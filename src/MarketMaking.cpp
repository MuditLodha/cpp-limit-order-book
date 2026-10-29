#include "market_making/Simulator.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <random>
#include <stdexcept>

namespace market_making {
using namespace matching_engine;
std::string name(Strategy s) {
    switch(s) {
        case Strategy::Fixed: return "fixed";
        case Strategy::Inventory: return "inventory";
        case Strategy::AvellanedaStoikov: return "as";
    }
    throw std::invalid_argument("Unknown strategy");
}
Strategy parseStrategy(const std::string& s) {
    if(s=="fixed") return Strategy::Fixed;
    if(s=="inventory") return Strategy::Inventory;
    if(s=="as") return Strategy::AvellanedaStoikov;
    throw std::invalid_argument("Strategy must be fixed, inventory or as");
}
void Config::validate() const {
    const double values[]={seconds,dt,initialPrice,sigma,arrivalRate,informedFraction,
        meanOrderSize,meanTolerance,halfSpread,skew,gamma,kappa,riskHorizon,
        makerFee,takerFee,markoutHorizon,initialCapital};
    for(double x:values) if(!std::isfinite(x))
        throw std::invalid_argument("All numeric inputs must be finite");
    if(dt<=0 || seconds<dt || seconds/dt>1000000 ||
       std::abs(seconds/dt-std::round(seconds/dt))>1e-7)
        throw std::invalid_argument("seconds must be an integer multiple of dt; 1..1e6 steps");
    if(sigma<0 || sigma>100 || arrivalRate<0 || arrivalRate*dt>1000 ||
       informedFraction<0 || informedFraction>1 || meanOrderSize<1 ||
       meanOrderSize>10000 || meanTolerance<=0 || halfSpread<0.5 ||
       halfSpread>100 || skew<0 || gamma<=0 || kappa<=0 || riskHorizon<=0)
        throw std::invalid_argument("Invalid market or quoting parameters");
    if(quoteSize<1 || inventoryLimit<1 || inventoryLimit>100000 ||
       backgroundLevels<1 || backgroundLevels>100 || backgroundSize<1 ||
       backgroundSize>100000 || backgroundSpacing<1 || backgroundSpacing>100 ||
       backgroundHalfSpread<1 || backgroundHalfSpread>100 ||
       static_cast<long long>(backgroundLevels)*backgroundSize<inventoryLimit)
        throw std::invalid_argument("Invalid sizes; background depth must cover inventory limit");
    if(initialPrice<1000 || initialPrice>19000 || makerFee<0 || takerFee<0 ||
       markoutHorizon<=0 || initialCapital<=0)
        throw std::invalid_argument("Invalid price, fees, horizon or capital");
}
Quote makeQuote(const Config& c,double s,int q,double remaining) {
    double center=s,half=c.halfSpread;
    if(c.strategy==Strategy::Inventory) center-=c.skew*q;
    if(c.strategy==Strategy::AvellanedaStoikov) {
        // Rolling A-S approximation. Actual engine fills do not follow the
        // paper's exponential intensity law, so this is not claimed optimal.
        const double tau=std::min(c.riskHorizon,std::max(0.0,remaining));
        const double risk=c.gamma*c.sigma*c.sigma*tau;
        center-=q*risk;
        half=0.5*risk+std::log1p(c.gamma/c.kappa)/c.gamma;
    }
    if(!std::isfinite(center) || !std::isfinite(half) ||
       center-half<1 || center+half>20000)
        throw std::runtime_error("Quote exceeds finite ladder; reduce parameters");
    return {static_cast<int>(std::floor(center-half)),
            static_cast<int>(std::ceil(center+half)),
            std::max(0,std::min(c.quoteSize,c.inventoryLimit-q)),
            std::max(0,std::min(c.quoteSize,c.inventoryLimit+q))};
}
std::vector<Tick> generateTape(const Config& c) {
    c.validate();
    std::mt19937_64 priceRng(c.seed),flowRng(c.seed^0x9e3779b97f4a7c15ULL);
    std::normal_distribution<double> normal(0,1);
    std::poisson_distribution<int> arrivals(c.arrivalRate*c.dt);
    std::geometric_distribution<int> size(1.0/c.meanOrderSize);
    std::exponential_distribution<double> tolerance(1.0/c.meanTolerance);
    std::uniform_real_distribution<double> uniform(0,1);
    const auto count=static_cast<size_t>(std::llround(c.seconds/c.dt));
    std::vector<Tick> tape; tape.reserve(count);
    for(size_t i=0;i<count;++i) {
        Tick tick{c.sigma*std::sqrt(c.dt)*normal(priceRng),{}};
        int n=arrivals(flowRng);
        for(int j=0;j<n;++j) {
            // Draw every field regardless of scenario: strategies get the same tape.
            const bool informed=uniform(flowRng)<c.informedFraction;
            const bool noiseBuy=uniform(flowRng)<0.5;
            const bool buy=informed && tick.innovation!=0 ? tick.innovation>0 : noiseBuy;
            tick.orders.push_back({buy?Side::Buy:Side::Sell,
                1+std::min(size(flowRng),100000),tolerance(flowRng),informed});
        }
        tape.push_back(std::move(tick));
    }
    return tape;
}
void Account::fill(int direction,int quantity,double price,double feePerLot) {
    if((direction!=1 && direction!=-1) || quantity<=0 || price<=0 ||
       feePerLot<0 || !std::isfinite(price) || !std::isfinite(feePerLot))
        throw std::invalid_argument("Invalid fill");
    inventory+=direction*quantity;
    const double fee=quantity*feePerLot;
    cash-=direction*quantity*price+fee;
    fees+=fee;
}
double Result::fillRate() const {
    return postedVolume?static_cast<double>(passiveVolume)/postedVolume:0;
}
Result run(const Config& c) { return run(c,generateTape(c)); }
Result run(const Config& c,const std::vector<Tick>& tape) {
    c.validate();
    const size_t steps=static_cast<size_t>(std::llround(c.seconds/c.dt));
    if(tape.size()!=steps) throw std::invalid_argument("Tape length differs from duration");
    MatchingEngine engine(OrderBookConfig(1,20000,10000));
    Result r; r.config=c; Account account;
    OrderId nextId=1,bidId=0,askId=0,liquidationId=0;
    std::vector<OrderId> background;
    double reference=c.initialPrice,time=0;
    bool liquidating=false;
    engine.setRejectListener([](OrderId,RejectReason reason) {
        if(reason!=RejectReason::NoLiquidity)
            throw std::runtime_error("Unexpected matching-engine rejection");
    });
    engine.setTradeListener([&](const ExecutionReport& e) {
        int direction=0;
        if(e.makerOrderId==bidId) direction=1;
        if(e.makerOrderId==askId) direction=-1;
        if(e.takerOrderId==liquidationId && liquidating)
            direction=e.takerSide==Side::Buy?1:-1;
        if(!direction) return;
        const double fee=liquidating?c.takerFee:c.makerFee;
        account.fill(direction,static_cast<int>(e.matchQuantity),e.matchPrice,fee);
        r.fills.push_back({time,static_cast<double>(e.matchPrice),reference,
            fee*e.matchQuantity,direction,static_cast<int>(e.matchQuantity),liquidating,
            e.makerOrderId,e.takerOrderId});
        if(!liquidating) r.passiveVolume+=e.matchQuantity;
        if(std::abs(account.inventory)>c.inventoryLimit)
            throw std::runtime_error("Inventory limit breached");
    });
    auto cancelIfLive=[&](OrderId id) {
        if(id && engine.book().findOrder(id)) engine.cancelOrder(id);
    };
    auto cancelQuotes=[&]() { cancelIfLive(bidId);cancelIfLive(askId);bidId=askId=0; };
    auto refreshBackground=[&]() {
        for(OrderId id:background) cancelIfLive(id);
        background.clear();
        for(int level=0;level<c.backgroundLevels;++level) {
            const int distance=c.backgroundHalfSpread+level*c.backgroundSpacing;
            const int bid=static_cast<int>(std::floor(reference-distance));
            const int ask=static_cast<int>(std::ceil(reference+distance));
            if(bid<1 || ask>20000) throw std::runtime_error("Reference left price ladder");
            for(auto [side,price]:{std::pair{Side::Buy,bid},std::pair{Side::Sell,ask}}) {
                OrderId id=nextId++;
                if(!engine.submitLimitOrder(id,side,price,c.backgroundSize))
                    throw std::runtime_error("Background order rejected");
                background.push_back(id);
            }
        }
    };
    auto sample=[&](int bid,int ask) {
        r.samples.push_back({time,reference,account.cash,account.equity(reference),
                            account.inventory,bid,ask});
    };
    for(size_t i=0;i<steps;++i) {
        time=i*c.dt;
        cancelQuotes();
        refreshBackground(); // competitor receives FIFO priority at identical prices
        Quote quote=makeQuote(c,reference,account.inventory,c.seconds-time);
        // Passive-only policy: clamp aggressive inventory skew to the opposite BBO.
        quote.bid=std::min(quote.bid,static_cast<int>(engine.book().bestAskPrice())-1);
        quote.ask=std::max(quote.ask,static_cast<int>(engine.book().bestBidPrice())+1);
        if(quote.bid>=quote.ask) throw std::runtime_error("Crossed strategy quotes");
        const auto timestamp=static_cast<Timestamp>(std::llround(time*1e9));
        if(quote.buySize>0) {
            bidId=nextId++;
            engine.submitLimitOrder(bidId,Side::Buy,quote.bid,quote.buySize,timestamp);
            r.postedVolume+=quote.buySize;
        }
        if(quote.sellSize>0) {
            askId=nextId++;
            engine.submitLimitOrder(askId,Side::Sell,quote.ask,quote.sellSize,timestamp);
            r.postedVolume+=quote.sellSize;
        }
        sample(quote.buySize?quote.bid:0,quote.sellSize?quote.ask:0);
        // Strategy has acted before the hidden innovation/informed flags are accessed.
        const Tick& tick=tape[i];
        if(!std::isfinite(tick.innovation)) throw std::invalid_argument("Invalid innovation");
        for(const Flow& flow:tick.orders) {
            if(flow.quantity<=0 || !std::isfinite(flow.tolerance) || flow.tolerance<0)
                throw std::invalid_argument("Invalid incoming flow");
            const double raw=flow.side==Side::Buy?std::floor(reference+flow.tolerance)
                                                :std::ceil(reference-flow.tolerance);
            const Price limit=static_cast<Price>(std::clamp(raw,1.0,20000.0));
            OrderId id=nextId++;
            r.incomingVolume+=flow.quantity;
            engine.submitLimitOrder(id,flow.side,limit,flow.quantity,timestamp);
            // IOC: cancel remainder immediately. All fills came from the engine.
            if(auto* rest=engine.book().findOrder(id)) {
                r.unfilledIncomingVolume+=rest->remainingQuantity;
                engine.cancelOrder(id);
            }
        }
        reference+=tick.innovation;
        if(!std::isfinite(reference) || reference<100 || reference>19900)
            throw std::runtime_error("Price path outside supported ladder");
    }
    time=c.seconds;
    r.preLiquidationPnl=account.equity(reference);
    cancelQuotes();refreshBackground();
    if(account.inventory!=0) {
        liquidating=true;liquidationId=nextId++;
        const auto side=account.inventory>0?Side::Sell:Side::Buy;
        const Quantity quantity=static_cast<Quantity>(std::abs(account.inventory));
        if(engine.submitMarketOrder(liquidationId,side,quantity,
            static_cast<Timestamp>(std::llround(time*1e9)))!=quantity)
            throw std::runtime_error("Insufficient final liquidation depth");
    }
    sample(0,0);
    r.finalInventory=account.inventory;r.netPnl=account.cash;r.fees=account.fees;
    r.liquidationCost=r.preLiquidationPnl-r.netPnl;
    calculateMetrics(r);
    return r;
}
void calculateMetrics(Result& r) {
    const auto& c=r.config;
    const size_t n=r.samples.size()-1;
    double sumQ=0,sumQ2=0,sumAbs=0,peak=0,sumR=0,sumR2=0;
    r.maxDrawdown=0;r.maxAbsInventory=0;
    for(size_t i=0;i<r.samples.size();++i) {
        const auto& s=r.samples[i];
        peak=std::max(peak,s.equity);
        r.maxDrawdown=std::max(r.maxDrawdown,peak-s.equity);
        if(i<n) {
            sumQ+=s.inventory;sumQ2+=static_cast<double>(s.inventory)*s.inventory;
            sumAbs+=std::abs(s.inventory);
        }
        r.maxAbsInventory=std::max(r.maxAbsInventory,std::abs(s.inventory));
        if(i>0) {
            const double ret=(s.equity-r.samples[i-1].equity)/c.initialCapital;
            sumR+=ret;sumR2+=ret*ret;
        }
    }
    int q=0;
    for(const auto& f:r.fills) {
        q+=f.direction*f.quantity;
        r.maxAbsInventory=std::max(r.maxAbsInventory,std::abs(q));
    }
    const double mean=sumQ/n;
    r.inventoryVariance=std::max(0.0,sumQ2/n-mean*mean);
    r.meanAbsInventory=sumAbs/n;
    const double variance=n>1?std::max(0.0,(sumR2-sumR*sumR/n)/(n-1)):0;
    r.stepSharpe=variance>0?(sumR/n)/std::sqrt(variance):0;
    double spread=0,adverse=0,markout=0;
    r.markoutVolume=0;r.censoredMarkoutVolume=0;
    for(const auto& f:r.fills) {
        if(f.liquidation) continue;
        spread+=f.quantity*f.direction*(f.reference-f.price);
        const auto index=static_cast<size_t>(std::ceil((f.time+c.markoutHorizon)/c.dt-1e-9));
        if(index>=r.samples.size()) { r.censoredMarkoutVolume+=f.quantity;continue; }
        const double future=r.samples[index].reference;
        adverse+=-f.quantity*f.direction*(future-f.reference);
        markout+=f.quantity*f.direction*(future-f.price);
        r.markoutVolume+=f.quantity;
    }
    r.spreadCapture=r.passiveVolume?spread/r.passiveVolume:0;
    r.adverseSelection=r.markoutVolume?adverse/r.markoutVolume:0;
    r.executionMarkout=r.markoutVolume?markout/r.markoutVolume:0;
}
void writeSummaryHeader(std::ostream& o) {
    o<<"group,regime,strategy,seed,seconds,dt,sigma,informed_fraction,half_spread,quote_size,"
       "inventory_limit,net_pnl,pre_liquidation_pnl,liquidation_cost,fees,max_drawdown,"
       "inventory_variance,mean_abs_inventory,max_abs_inventory,step_sharpe,passive_volume,"
       "posted_volume,fill_rate,incoming_volume,unfilled_incoming_volume,spread_capture,"
       "adverse_selection,execution_markout,markout_volume,censored_markout_volume,final_inventory\n";
}
void writeSummary(std::ostream& o,const Result& r,const std::string& group,const std::string& regime) {
    const auto& c=r.config;
    o<<std::setprecision(12)<<group<<','<<regime<<','<<name(c.strategy)<<','<<c.seed<<','
     <<c.seconds<<','<<c.dt<<','<<c.sigma<<','<<c.informedFraction<<','<<c.halfSpread<<','
     <<c.quoteSize<<','<<c.inventoryLimit<<','<<r.netPnl<<','<<r.preLiquidationPnl<<','
     <<r.liquidationCost<<','<<r.fees<<','<<r.maxDrawdown<<','<<r.inventoryVariance<<','
     <<r.meanAbsInventory<<','<<r.maxAbsInventory<<','<<r.stepSharpe<<','<<r.passiveVolume<<','
     <<r.postedVolume<<','<<r.fillRate()<<','<<r.incomingVolume<<','<<r.unfilledIncomingVolume<<','
     <<r.spreadCapture<<','<<r.adverseSelection<<','<<r.executionMarkout<<','<<r.markoutVolume<<','
     <<r.censoredMarkoutVolume<<','<<r.finalInventory<<'\n';
}
void writeDetails(const Result& r,const std::string& directory) {
    std::filesystem::create_directories(directory);
    std::ofstream s(std::filesystem::path(directory)/"trajectory.csv");
    std::ofstream f(std::filesystem::path(directory)/"fills.csv");
    if(!s || !f) throw std::runtime_error("Cannot write detail CSV");
    s<<std::setprecision(12)<<"time,reference,cash,equity,inventory,bid,ask\n";
    for(const auto& x:r.samples)
        s<<x.time<<','<<x.reference<<','<<x.cash<<','<<x.equity<<','<<x.inventory<<','<<x.bid<<','<<x.ask<<'\n';
    f<<std::setprecision(12)<<"time,direction,quantity,price,reference,fee,liquidation,maker_id,taker_id\n";
    for(const auto& x:r.fills)
        f<<x.time<<','<<x.direction<<','<<x.quantity<<','<<x.price<<','<<x.reference<<','
         <<x.fee<<','<<x.liquidation<<','<<x.makerId<<','<<x.takerId<<'\n';
    if(!s.good() || !f.good()) throw std::runtime_error("Failed writing detail CSV");
}
} // namespace market_making
