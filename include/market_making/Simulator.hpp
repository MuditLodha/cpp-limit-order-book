#pragma once
#include "matching_engine/MatchingEngine.hpp"
#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

// Prices are ticks; cash/PnL are tick-lots. No connectivity or live orders.
namespace market_making {
enum class Strategy { Fixed, Inventory, AvellanedaStoikov };
std::string name(Strategy strategy);
Strategy parseStrategy(const std::string& value);
struct Config {
    Strategy strategy = Strategy::Inventory;
    std::uint64_t seed = 1;
    double seconds = 120.0, dt = 0.1, initialPrice = 10000.0;
    double sigma = 1.5; // ticks / sqrt(second)
    double arrivalRate = 8.0, informedFraction = 0.0;
    double meanOrderSize = 12.0, meanTolerance = 4.0;
    double halfSpread = 2.0, skew = 0.12;
    double gamma = 0.02, kappa = 0.5, riskHorizon = 2.0;
    int quoteSize = 5, inventoryLimit = 50;
    int backgroundHalfSpread = 3, backgroundSpacing = 2;
    int backgroundLevels = 10, backgroundSize = 8;
    double makerFee = 0.02, takerFee = 0.10; // ticks per filled lot
    double markoutHorizon = 1.0, initialCapital = 1000000.0;
    void validate() const;
};
struct Quote { int bid = 0, ask = 0, buySize = 0, sellSize = 0; };
Quote makeQuote(const Config& c, double reference, int inventory, double remaining);
struct Flow { matching_engine::Side side; int quantity; double tolerance; bool informed; };
struct Tick { double innovation; std::vector<Flow> orders; };
std::vector<Tick> generateTape(const Config& c);
struct Account {
    int inventory = 0;
    double cash = 0.0, fees = 0.0;
    void fill(int direction, int quantity, double price, double feePerLot);
    double equity(double reference) const { return cash + inventory * reference; }
};
struct Fill {
    double time, price, reference, fee;
    int direction, quantity;
    bool liquidation;
    std::uint64_t makerId, takerId;
};
struct Sample { double time, reference, cash, equity; int inventory, bid, ask; };
struct Result {
    Config config;
    std::vector<Sample> samples;
    std::vector<Fill> fills;
    double netPnl = 0, preLiquidationPnl = 0, liquidationCost = 0;
    double fees = 0, maxDrawdown = 0, inventoryVariance = 0;
    double meanAbsInventory = 0, stepSharpe = 0;
    double spreadCapture = 0, adverseSelection = 0, executionMarkout = 0;
    int maxAbsInventory = 0, finalInventory = 0;
    std::uint64_t passiveVolume = 0, postedVolume = 0;
    std::uint64_t incomingVolume = 0, unfilledIncomingVolume = 0;
    std::uint64_t markoutVolume = 0, censoredMarkoutVolume = 0;
    double fillRate() const;
};
Result run(const Config& config);
Result run(const Config& config, const std::vector<Tick>& tape);
void calculateMetrics(Result& result);
void writeSummaryHeader(std::ostream& out);
void writeSummary(std::ostream& out, const Result& r, const std::string& group,
                  const std::string& regime);
void writeDetails(const Result& r, const std::string& directory);
} // namespace market_making
