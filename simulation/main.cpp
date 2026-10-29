#include "market_making/Simulator.hpp"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

using namespace market_making;
namespace fs=std::filesystem;
double number(const std::string& s) {
    size_t n=0; double x=std::stod(s,&n);
    if(n!=s.size() || !std::isfinite(x)) throw std::invalid_argument("Invalid number: "+s);
    return x;
}
int integer(const std::string& s) {
    double x=number(s);
    if(std::floor(x)!=x || x<std::numeric_limits<int>::min() ||
       x>std::numeric_limits<int>::max()) throw std::invalid_argument("Expected integer: "+s);
    return static_cast<int>(x);
}
void metadata(const fs::path& path,const Config& c,bool study,int seeds) {
    std::ofstream o(path);
    if(!o) throw std::runtime_error("Cannot write metadata");
    o<<"{\n  \"model_version\": 1,\n  \"study\": "<<(study?"true":"false")
     <<",\n  \"seeds\": "<<seeds<<",\n  \"first_seed\": "<<c.seed
     <<",\n  \"seconds\": "<<c.seconds<<",\n  \"dt\": "<<c.dt
     <<",\n  \"arrival_rate\": "<<c.arrivalRate
     <<",\n  \"mean_order_size\": "<<c.meanOrderSize
     <<",\n  \"mean_tolerance\": "<<c.meanTolerance
     <<",\n  \"skew\": "<<c.skew<<",\n  \"gamma\": "<<c.gamma
     <<",\n  \"kappa\": "<<c.kappa<<",\n  \"risk_horizon\": "<<c.riskHorizon
     <<",\n  \"maker_fee\": "<<c.makerFee<<",\n  \"taker_fee\": "<<c.takerFee
     <<",\n  \"initial_price\": "<<c.initialPrice
     <<",\n  \"initial_capital\": "<<c.initialCapital
     <<",\n  \"markout_horizon\": "<<c.markoutHorizon
     <<",\n  \"background_half_spread\": "<<c.backgroundHalfSpread
     <<",\n  \"background_spacing\": "<<c.backgroundSpacing
     <<",\n  \"background_levels\": "<<c.backgroundLevels
     <<",\n  \"background_size\": "<<c.backgroundSize
     <<",\n  \"compiler\": \""<<__VERSION__<<"\","
       "\n  \"units\": \"prices=ticks; quantities=lots; PnL=tick-lots\","
       "\n  \"randomness\": \"mt19937_64; distributions reproducible with same C++ library\","
       "\n  \"flow\": \"Poisson counts per dt; marketable limit IOC; synthetic\"\n}\n";
}
int main(int argc,char** argv) {
    try {
        Config c;
        bool study=false;
        int seeds=12;
        fs::path output="results/single";
        for(int i=1;i<argc;++i) {
            std::string arg=argv[i];
            if(arg=="--help") {
                std::cout<<"Market-making research simulator (synthetic; no live trading)\n"
                    <<"  --strategy fixed|inventory|as --seed N --seconds N --dt N\n"
                    <<"  --sigma N --informed N --rate N --half-spread N --quote-size N\n"
                    <<"  --limit N --skew N --gamma N --kappa N --risk-horizon N\n"
                    <<"  --maker-fee N --taker-fee N --markout-horizon N --output DIR\n"
                    <<"  --study --seeds N : 4 regimes x 3 strategies + width/size sweep\n"
                    <<"Study fixes regimes, spreads and sizes; other options apply to all runs.\n";
                return 0;
            }
            if(arg=="--study") { study=true;continue; }
            if(i+1>=argc) throw std::invalid_argument("Missing value for "+arg);
            std::string value=argv[++i];
            if(arg=="--strategy") c.strategy=parseStrategy(value);
            else if(arg=="--seed") {
                int s=integer(value);
                if(s<0) throw std::invalid_argument("Seed must be nonnegative");
                c.seed=static_cast<std::uint64_t>(s);
            }
            else if(arg=="--seconds") c.seconds=number(value);
            else if(arg=="--dt") c.dt=number(value);
            else if(arg=="--sigma") c.sigma=number(value);
            else if(arg=="--informed") c.informedFraction=number(value);
            else if(arg=="--rate") c.arrivalRate=number(value);
            else if(arg=="--half-spread") c.halfSpread=number(value);
            else if(arg=="--quote-size") c.quoteSize=integer(value);
            else if(arg=="--limit") c.inventoryLimit=integer(value);
            else if(arg=="--skew") c.skew=number(value);
            else if(arg=="--gamma") c.gamma=number(value);
            else if(arg=="--kappa") c.kappa=number(value);
            else if(arg=="--risk-horizon") c.riskHorizon=number(value);
            else if(arg=="--maker-fee") c.makerFee=number(value);
            else if(arg=="--taker-fee") c.takerFee=number(value);
            else if(arg=="--markout-horizon") c.markoutHorizon=number(value);
            else if(arg=="--seeds") seeds=integer(value);
            else if(arg=="--output") output=value;
            else throw std::invalid_argument("Unknown argument: "+arg);
        }
        c.validate();
        if(seeds<1 || seeds>1000 || (study && seeds<2))
            throw std::invalid_argument("seeds must be 2..1000 for a study (1..1000 otherwise)");
        fs::create_directories(output);
        metadata(output/"metadata.json",c,study,seeds);
        std::ofstream summary(output/"runs.csv");
        if(!summary) throw std::runtime_error("Cannot write runs.csv");
        writeSummaryHeader(summary);
        if(!study) {
            auto r=run(c);writeSummary(summary,r,"single","custom");
            writeDetails(r,output.string());
            std::cout<<"Net PnL: "<<r.netPnl<<" tick-lots; max |inventory|: "
                     <<r.maxAbsInventory<<"; final inventory: "<<r.finalInventory
                     <<"; filled/posted volume: "<<100*r.fillRate()<<"%\n";
        } else {
            struct Regime { const char* label;double sigma,informed; };
            const Regime regimes[]={{"quiet",0.5,0},{"normal",1.5,0},
                                     {"volatile",3.0,0},{"informed",1.5,0.6}};
            int completed=0;
            auto execute=[&](Config config,const char* group,const char* regime,bool details) {
                auto r=run(config);writeSummary(summary,r,group,regime);
                if(details) writeDetails(r,(output/"examples"/regime/name(config.strategy)).string());
                if(++completed%24==0) std::cout<<"Completed "<<completed<<" runs\n"<<std::flush;
            };
            for(auto regime:regimes)
                for(int seed=0;seed<seeds;++seed)
                    for(auto strategy:{Strategy::Fixed,Strategy::Inventory,Strategy::AvellanedaStoikov}) {
                        Config x=c;x.strategy=strategy;x.seed=c.seed+seed;
                        x.sigma=regime.sigma;x.informedFraction=regime.informed;
                        x.halfSpread=2;x.quoteSize=5;
                        execute(x,"comparison",regime.label,seed==0);
                    }
            // Fixed and linear-skew strategies only: A-S determines its own spread.
            for(auto regime:{regimes[2],regimes[3]})
                for(int width:{1,2,4}) for(int size:{2,5,10})
                    for(int seed=0;seed<seeds;++seed)
                        for(auto strategy:{Strategy::Fixed,Strategy::Inventory}) {
                            Config x=c;x.strategy=strategy;x.seed=c.seed+seed;
                            x.sigma=regime.sigma;x.informedFraction=regime.informed;
                            x.halfSpread=width;x.quoteSize=size;
                            execute(x,"sweep",regime.label,false);
                        }
            std::cout<<"Completed all "<<completed<<" runs.\n";
        }
        if(!summary.good()) throw std::runtime_error("Failed writing summary");
        std::cout<<"Results: "<<fs::absolute(output).string()<<"\n";
        return 0;
    } catch(const std::exception& e) {
        std::cerr<<"ERROR: "<<e.what()<<"\n";return 1;
    }
}
