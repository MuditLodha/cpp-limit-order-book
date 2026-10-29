#include "market_making/Simulator.hpp"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace market_making;
using namespace matching_engine;
#define CHECK(x) do { if(!(x)) { std::cerr<<"FAILED "<<#x<<" line "<<__LINE__<<"\n"; std::exit(1); } } while(0)
bool near(double a,double b) { return std::abs(a-b)<1e-7; }
template<class F> void throws(F f) { bool caught=false;try { f(); } catch(const std::exception&) { caught=true; } CHECK(caught); }
void accounting() {
    Account a;a.fill(1,5,100,0.1);
    CHECK(a.inventory==5);CHECK(near(a.cash,-500.5));CHECK(near(a.equity(102),9.5));
    a.fill(-1,5,103,0.2);
    CHECK(a.inventory==0);CHECK(near(a.cash,13.5));CHECK(near(a.fees,1.5));
    Account shortAccount;shortAccount.fill(-1,4,100,0);shortAccount.fill(1,4,98,0);
    CHECK(near(shortAccount.cash,8));CHECK(shortAccount.inventory==0);
}
void policies() {
    Config c;auto neutral=makeQuote(c,10000,0,10);
    auto longQ=makeQuote(c,10000,10,10);
    CHECK(longQ.bid<neutral.bid);CHECK(longQ.ask<neutral.ask);
    CHECK(makeQuote(c,10000,50,10).buySize==0);
    CHECK(makeQuote(c,10000,-50,10).sellSize==0);
    CHECK(makeQuote(c,10000,48,10).buySize==2);
    c.strategy=Strategy::Fixed;
    CHECK(makeQuote(c,10000,10,10).bid==makeQuote(c,10000,0,10).bid);
    c.strategy=Strategy::AvellanedaStoikov;
    auto low=makeQuote(c,10000,0,10);c.sigma=8;
    auto high=makeQuote(c,10000,0,10);
    CHECK(high.ask-high.bid>low.ask-low.bid);
    CHECK(makeQuote(c,10000,10,10).bid<high.bid);
}
void fifoAndIoc() {
    Config c;c.seconds=c.dt;c.sigma=0;c.strategy=Strategy::Fixed;
    c.halfSpread=3;c.quoteSize=5;c.markoutHorizon=c.dt;
    std::vector<Tick> t{{0,{{Side::Sell,10,3,false}}}};
    auto r=run(c,t);
    // Eight lots of background at 9997 are ahead of us. We receive only two.
    CHECK(r.passiveVolume==2);CHECK(r.fills.size()==2);
    CHECK(r.fills[0].quantity==2);CHECK(r.fills[0].direction==1);
    CHECK(r.fills[0].makerId!=0);CHECK(r.finalInventory==0);
    CHECK(near(r.netPnl,-0.24)); // bought 2 @ 9997, sold 2 @ 9997, both fees
    CHECK(near(r.liquidationCost,6.2));
    t[0].orders[0].quantity=8;r=run(c,t);CHECK(r.passiveVolume==0);
    t[0].orders[0].quantity=100;t[0].orders[0].tolerance=0;
    r=run(c,t);CHECK(r.passiveVolume==0);CHECK(r.unfilledIncomingVolume==100);
}
void capsAndLiquidation() {
    Config c;c.seconds=1;c.dt=0.1;c.strategy=Strategy::Fixed;
    c.quoteSize=40;c.inventoryLimit=50;c.sigma=0;
    std::vector<Tick> tape(10,Tick{0,{{Side::Sell,200,100,false}}});
    auto r=run(c,tape);
    CHECK(r.passiveVolume==50);CHECK(r.maxAbsInventory==50);CHECK(r.finalInventory==0);
    CHECK(r.liquidationCost>0);
    double cash=0,fees=0;int q=0;
    for(auto f:r.fills) { cash-=f.direction*f.quantity*f.price+f.fee;q+=f.direction*f.quantity;fees+=f.fee; }
    CHECK(q==0);CHECK(near(cash,r.netPnl));CHECK(near(fees,r.fees));
    for(auto& tick:tape) tick.orders[0].side=Side::Buy;
    auto shortResult=run(c,tape);CHECK(shortResult.maxAbsInventory==50);
    CHECK(shortResult.finalInventory==0);CHECK(shortResult.passiveVolume==50);
}
void markouts() {
    Config c;c.seconds=0.2;c.dt=0.1;c.markoutHorizon=0.1;c.sigma=0;c.strategy=Strategy::Fixed;
    std::vector<Tick> t{{-2,{{Side::Sell,1,2,true}}},{0,{}}};
    auto r=run(c,t);
    CHECK(r.passiveVolume==1);CHECK(r.markoutVolume==1);
    CHECK(near(r.adverseSelection,2));CHECK(near(r.spreadCapture,2));
    CHECK(near(r.executionMarkout,0));
    c.markoutHorizon=1;r=run(c,t);
    CHECK(r.markoutVolume==0);CHECK(r.censoredMarkoutVolume==1);
}
void reproducibilityAndFairness() {
    Config c;c.seconds=2;
    auto tape=generateTape(c),other=generateTape(c);
    CHECK(tape.size()==other.size());
    for(size_t i=0;i<tape.size();++i) {
        CHECK(tape[i].innovation==other[i].innovation);
        CHECK(tape[i].orders.size()==other[i].orders.size());
    }
    auto a=run(c,tape),b=run(c,tape);
    CHECK(a.netPnl==b.netPnl);CHECK(a.fills.size()==b.fills.size());
    c.strategy=Strategy::Fixed;auto d=run(c);
    for(size_t i=0;i<a.samples.size();++i) CHECK(a.samples[i].reference==d.samples[i].reference);
    // Changing toxicity retains the exact price path and arrival sizes/tolerances.
    c.informedFraction=1;auto informed=generateTape(c);
    for(size_t i=0;i<tape.size();++i) {
        CHECK(tape[i].innovation==informed[i].innovation);
        CHECK(tape[i].orders.size()==informed[i].orders.size());
        for(size_t j=0;j<tape[i].orders.size();++j) {
            CHECK(tape[i].orders[j].quantity==informed[i].orders[j].quantity);
            CHECK(tape[i].orders[j].tolerance==informed[i].orders[j].tolerance);
            CHECK((informed[i].orders[j].side==Side::Buy)==(informed[i].innovation>0));
        }
    }
}
void causalQuotesAndCosts() {
    Config c;c.seconds=3;c.skew=0;
    auto tape=generateTape(c);
    auto baseline=run(c,tape);
    c.strategy=Strategy::Fixed;auto fixed=run(c,tape);
    CHECK(baseline.netPnl==fixed.netPnl);CHECK(baseline.fills.size()==fixed.fills.size());
    auto future=tape;future[0].innovation+=5;
    auto changedFuture=run(c,future);
    CHECK(fixed.samples[0].bid==changedFuture.samples[0].bid);
    CHECK(fixed.samples[0].ask==changedFuture.samples[0].ask);
    c.makerFee+=0.25;c.takerFee+=0.25;
    auto costly=run(c,tape);
    double quantity=0;
    for(const auto& f:fixed.fills) quantity+=f.quantity;
    CHECK(costly.fills.size()==fixed.fills.size());
    CHECK(near(fixed.netPnl-costly.netPnl,0.25*quantity));
}
void invalidAndEmpty() {
    Config c;c.seconds=1;c.arrivalRate=0;
    auto r=run(c);
    CHECK(r.netPnl==0);CHECK(r.fees==0);CHECK(r.stepSharpe==0);CHECK(r.fillRate()==0);
    c.dt=0;throws([&] { c.validate(); });
    c=Config{};c.sigma=std::numeric_limits<double>::quiet_NaN();throws([&] { c.validate(); });
    c=Config{};c.informedFraction=1.1;throws([&] { c.validate(); });
    c=Config{};c.inventoryLimit=1000;throws([&] { c.validate(); });
    throws([] { parseStrategy("bad"); });
}
int main() {
    accounting();policies();fifoAndIoc();capsAndLiquidation();markouts();
    reproducibilityAndFairness();causalQuotesAndCosts();invalidAndEmpty();
    std::cout<<"All 8 market-making test groups passed.\n";
}
