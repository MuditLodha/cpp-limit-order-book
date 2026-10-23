#include "matching_engine/MatchingEngine.hpp"
#include <iostream>
#include <fstream>
#include <iomanip>
#include <string>
#include <sstream>
#include <vector>
#include <algorithm>
#include <cctype>

using namespace matching_engine;

// The program's results are written to viewer.txt so the terminal can be used
// only for entering commands. The original terminal output buffer is kept for
// the short prompt shown while interactive commands are being entered.
static std::streambuf* gConsoleOutputBuffer = nullptr;

static void writeToTerminal(const std::string& text) {
    if (gConsoleOutputBuffer == nullptr) {
        return;
    }
    std::ostream terminal(gConsoleOutputBuffer);
    terminal << text;
    terminal.flush();
}

class ViewerOutputRedirect {
public:
    ViewerOutputRedirect()
        : file_("viewer.txt", std::ios::out | std::ios::trunc),
          originalCout_(std::cout.rdbuf()),
          originalCerr_(std::cerr.rdbuf()),
          active_(false) {
        gConsoleOutputBuffer = originalCout_;
        if (file_.is_open()) {
            std::cout.rdbuf(file_.rdbuf());
            std::cerr.rdbuf(file_.rdbuf());
            active_ = true;
        }
    }

    ~ViewerOutputRedirect() {
        flush();
        if (active_) {
            std::cout.rdbuf(originalCout_);
            std::cerr.rdbuf(originalCerr_);
        }
        gConsoleOutputBuffer = nullptr;
    }

    bool isOpen() const {
        return file_.is_open() && active_;
    }

    void flush() {
        std::cout.flush();
        std::cerr.flush();
        file_.flush();
    }

private:
    std::ofstream file_;
    std::streambuf* originalCout_;
    std::streambuf* originalCerr_;
    bool active_;
};

// Convert string to lower-case for case-insensitive command matching
static std::string toLower(std::string str) {
    std::transform(str.begin(), str.end(), str.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return str;
}

// Map RejectReason to human-readable string
static const char* rejectReasonToString(RejectReason reason) {
    switch (reason) {
        case RejectReason::None: return "None";
        case RejectReason::DuplicateOrderId: return "Duplicate Order ID";
        case RejectReason::OrderNotFound: return "Order Not Found";
        case RejectReason::InvalidPrice: return "Invalid Price";
        case RejectReason::InvalidQuantity: return "Invalid Quantity";
        case RejectReason::NoLiquidity: return "No Liquidity";
        case RejectReason::PriceOutOfRange: return "Price Out of Range";
        case RejectReason::CapacityExceeded: return "Capacity Exceeded";
        case RejectReason::InvalidOrderId: return "Invalid Order ID";
        default: return "Unknown Reason";
    }
}

// Print visual Order Book depth ladder
void printOrderBookDepth(const OrderBook& book, size_t depth = 5) {
    auto asks = book.getAsks(depth);
    auto bids = book.getBids(depth);

    std::cout << "\n+-------------------------------------------------------------+\n";
    std::cout << "|                      ORDER BOOK DEPTH                       |\n";
    std::cout << "+-------------------------------------------------------------+\n";
    std::cout << "|   ORDERS   |    ASK QTY   |    PRICE     |    BID QTY   | ORDERS |\n";
    std::cout << "+------------+--------------+--------------+--------------+--------+\n";

    // Print asks in reverse so highest ask is on top, best ask is right above spread
    for (auto it = asks.rbegin(); it != asks.rend(); ++it) {
        std::cout << "| " << std::setw(10) << it->orderCount
                  << " | " << std::setw(12) << it->volume
                  << " | " << std::setw(12) << it->price
                  << " |              |        |\n";
    }

    std::cout << "+------------+--------------+--------------+--------------+--------+\n";
    if (book.bestBidPrice() != INVALID_PRICE && book.bestAskPrice() != INVALID_PRICE) {
        const double spread = static_cast<double>(book.bestAskPrice() - book.bestBidPrice());
        std::cout << "| SPREAD: " << std::setw(6) << spread
                  << " ticks                                                |\n";
    } else {
        std::cout << "| SPREAD: OPEN / UNQUOTED                                     |\n";
    }
    std::cout << "+------------+--------------+--------------+--------------+--------+\n";

    // Print bids in descending order (best bid at top)
    for (const auto& level : bids) {
        std::cout << "|            |              | " << std::setw(12) << level.price
                  << " | " << std::setw(12) << level.volume
                  << " | " << std::setw(6) << level.orderCount << " |\n";
    }
    std::cout << "+-------------------------------------------------------------+\n";
}

// Seed the order book with initial resting liquidity
void seedOrderBook(MatchingEngine& engine) {
    std::cout << "\n[SEEDING] Populating initial resting orders...\n";
    engine.submitLimitOrder(101, Side::Buy, 100, 50);
    engine.submitLimitOrder(102, Side::Buy, 99, 100);
    engine.submitLimitOrder(103, Side::Buy, 98, 200);

    engine.submitLimitOrder(201, Side::Sell, 102, 40);
    engine.submitLimitOrder(202, Side::Sell, 103, 80);
    engine.submitLimitOrder(203, Side::Sell, 104, 150);
    std::cout << "[SUCCESS] Seeded 3 Bids (98-100) and 3 Asks (102-104).\n";
}

// Run the full 5-step guided scenario walkthrough
void runDemo(MatchingEngine& engine) {
    std::cout << "\n===============================================================\n";
    std::cout << "   High-Performance C++ Limit Order Book & Matching Engine     \n";
    std::cout << "   Price-Time Priority (FIFO) | Zero-Allocation Intrusive List \n";
    std::cout << "===============================================================\n";

    engine.reset();
    std::cout << "\n1. Seeding Order Book with initial resting liquidity...\n";
    seedOrderBook(engine);
    printOrderBookDepth(engine.book());

    std::cout << "\n2. Submitting aggressive Limit Buy Order #301 for 60 shares @ 103...\n";
    std::cout << "   (Crosses spread, matches 40 @ 102 and 20 @ 103)\n";
    engine.submitLimitOrder(301, Side::Buy, 103, 60);
    printOrderBookDepth(engine.book());

    std::cout << "\n3. Submitting Market Sell Order #401 for 75 shares...\n";
    std::cout << "   (Sweeps best bid 50 @ 100 and 25 @ 99)\n";
    engine.submitMarketOrder(401, Side::Sell, 75);
    printOrderBookDepth(engine.book());

    std::cout << "\n4. Modifying Order #103 (reduce quantity 200 -> 80)...\n";
    std::cout << "   (Quantity decrease preserves time priority)\n";
    engine.modifyOrder(103, 98, 80);
    printOrderBookDepth(engine.book());

    std::cout << "\n5. Canceling Order #203 @ 104...\n";
    engine.cancelOrder(203);
    printOrderBookDepth(engine.book());

    std::cout << "\nEngine demonstration finished successfully!\n";
}

// Print trading command cheat-sheet
void printHelp() {
    std::cout << "\n===============================================================\n";
    std::cout << "                  TRADING COMMANDS REFERENCE                   \n";
    std::cout << "===============================================================\n";
    std::cout << "COMMANDS:\n";
    std::cout << "  limit  <buy|sell> <price> <qty> [id]   Place a limit order\n";
    std::cout << "  market <buy|sell> <qty> [id]          Place a market order\n";
    std::cout << "  cancel <order_id>                     Cancel resting order\n";
    std::cout << "  modify <order_id> <price> <qty>       Modify price/quantity\n";
    std::cout << "  book   [depth]                        Display order book ladder (default: 5)\n";
    std::cout << "  seed                                  Seed book with sample liquidity\n";
    std::cout << "  demo                                  Run full guided 5-step walkthrough\n";
    std::cout << "  reset                                 Clear order book\n";
    std::cout << "  help                                  Show this help menu\n";
    std::cout << "  exit / quit                           Exit interactive mode\n";
    std::cout << "\nEXAMPLES:\n";
    std::cout << "  limit buy 100 50 101                  (Buy 50 @ 100 ticks, ID 101)\n";
    std::cout << "  limit sell 105 25                     (Sell 25 @ 105 ticks, auto ID)\n";
    std::cout << "  market buy 30                         (Market buy 30 shares)\n";
    std::cout << "  cancel 101                            (Cancel Order #101)\n";
    std::cout << "  modify 101 99 40                      (Change #101 to 40 @ 99)\n";
    std::cout << "  book 10                               (Display top 10 depth levels)\n";
    std::cout << "===============================================================\n\n";
}

// Split a line into tokens
static std::vector<std::string> tokenize(const std::string& line) {
    std::vector<std::string> tokens;
    std::istringstream iss(line);
    std::string token;
    while (iss >> token) {
        tokens.push_back(token);
    }
    return tokens;
}

// Execute a single command token list
// Returns false if user requested to exit/quit
bool processCommand(MatchingEngine& engine, const std::vector<std::string>& tokens, OrderId& nextAutoId) {
    if (tokens.empty()) return true;

    std::string cmd = toLower(tokens[0]);

    if (cmd == "exit" || cmd == "quit" || cmd == "q") {
        return false;
    }

    if (cmd == "help" || cmd == "--help" || cmd == "-h" || cmd == "?") {
        printHelp();
        return true;
    }

    if (cmd == "demo") {
        runDemo(engine);
        return true;
    }

    if (cmd == "seed") {
        seedOrderBook(engine);
        printOrderBookDepth(engine.book());
        return true;
    }

    if (cmd == "reset") {
        engine.reset();
        std::cout << "[RESET] Order book and memory pool cleared.\n";
        return true;
    }

    if (cmd == "book" || cmd == "depth") {
        size_t depth = 5;
        if (tokens.size() >= 2) {
            try {
                depth = std::stoul(tokens[1]);
            } catch (...) {
                depth = 5;
            }
        }
        printOrderBookDepth(engine.book(), depth);
        return true;
    }

    if (cmd == "limit") {
        if (tokens.size() < 4) {
            std::cout << "[ERROR] Usage: limit <buy|sell> <price> <qty> [id]\n";
            return true;
        }

        std::string sideStr = toLower(tokens[1]);
        Side side = (sideStr == "buy" || sideStr == "b") ? Side::Buy : Side::Sell;
        if (sideStr != "buy" && sideStr != "b" && sideStr != "sell" && sideStr != "s") {
            std::cout << "[ERROR] Invalid side '" << tokens[1] << "'. Must be 'buy' or 'sell'.\n";
            return true;
        }

        try {
            Price price = static_cast<Price>(std::stoul(tokens[2]));
            Quantity qty = static_cast<Quantity>(std::stoul(tokens[3]));
            OrderId id = (tokens.size() >= 5) ? std::stoull(tokens[4]) : nextAutoId++;

            std::cout << "\n>> Submitting Limit " << sideToString(side)
                      << " Order #" << id << ": " << qty << " @ " << price << " ticks\n";

            bool ok = engine.submitLimitOrder(id, side, price, qty);
            if (ok) {
                std::cout << ">> [ACCEPTED] Order #" << id << " processed.\n";
            }
        } catch (const std::exception& e) {
            std::cout << "[ERROR] Failed to parse limit order parameters: " << e.what() << "\n";
        }
        return true;
    }

    if (cmd == "market") {
        if (tokens.size() < 3) {
            std::cout << "[ERROR] Usage: market <buy|sell> <qty> [id]\n";
            return true;
        }

        std::string sideStr = toLower(tokens[1]);
        Side side = (sideStr == "buy" || sideStr == "b") ? Side::Buy : Side::Sell;
        if (sideStr != "buy" && sideStr != "b" && sideStr != "sell" && sideStr != "s") {
            std::cout << "[ERROR] Invalid side '" << tokens[1] << "'. Must be 'buy' or 'sell'.\n";
            return true;
        }

        try {
            Quantity qty = static_cast<Quantity>(std::stoul(tokens[2]));
            OrderId id = (tokens.size() >= 4) ? std::stoull(tokens[3]) : nextAutoId++;

            std::cout << "\n>> Submitting Market " << sideToString(side)
                      << " Order #" << id << ": " << qty << " shares\n";

            Quantity filled = engine.submitMarketOrder(id, side, qty);
            std::cout << ">> [MARKET SUMMARY] Total Filled: " << filled << " / " << qty << "\n";
            if (filled < qty) {
                std::cout << ">> [UNFILLED] " << (qty - filled) << " shares expired (book exhausted).\n";
            }
        } catch (const std::exception& e) {
            std::cout << "[ERROR] Failed to parse market order parameters: " << e.what() << "\n";
        }
        return true;
    }

    if (cmd == "cancel") {
        if (tokens.size() < 2) {
            std::cout << "[ERROR] Usage: cancel <order_id>\n";
            return true;
        }

        try {
            OrderId id = std::stoull(tokens[1]);
            std::cout << "\n>> Canceling Order #" << id << "...\n";
            bool ok = engine.cancelOrder(id);
            if (!ok) {
                std::cout << ">> [ERROR] Order #" << id << " not found or already filled.\n";
            }
        } catch (const std::exception& e) {
            std::cout << "[ERROR] Failed to parse order ID: " << e.what() << "\n";
        }
        return true;
    }

    if (cmd == "modify") {
        if (tokens.size() < 4) {
            std::cout << "[ERROR] Usage: modify <order_id> <new_price> <new_qty>\n";
            return true;
        }

        try {
            OrderId id = std::stoull(tokens[1]);
            Price newPrice = static_cast<Price>(std::stoul(tokens[2]));
            Quantity newQty = static_cast<Quantity>(std::stoul(tokens[3]));

            std::cout << "\n>> Modifying Order #" << id << " -> Qty: " << newQty << " @ Price: " << newPrice << "...\n";
            bool ok = engine.modifyOrder(id, newPrice, newQty);
            if (!ok) {
                std::cout << ">> [ERROR] Order #" << id << " could not be modified (not found or filled).\n";
            } else {
                std::cout << ">> [MODIFIED] Order #" << id << " updated successfully.\n";
            }
        } catch (const std::exception& e) {
            std::cout << "[ERROR] Failed to parse modify parameters: " << e.what() << "\n";
        }
        return true;
    }

    std::cout << "[ERROR] Unknown command '" << tokens[0] << "'. Type 'help' to see valid commands.\n";
    return true;
}

// Start interactive REPL trading session
void runInteractiveSession(MatchingEngine& engine, OrderId& nextAutoId) {
    std::cout << "\n===============================================================\n";
    std::cout << "   High-Performance C++ Limit Order Book & Matching Engine     \n";
    std::cout << "   Interactive Trading Terminal | Price-Time Priority (FIFO)   \n";
    std::cout << "===============================================================\n";
    std::cout << "Type 'help' for commands, 'demo' for walkthrough, 'exit' to quit.\n\n";

    std::string line;
    while (true) {
        // Keep the input prompt visible in the terminal; command results go
        // only to viewer.txt.
        std::cout.flush();
        std::cerr.flush();
        writeToTerminal("lob> ");

        if (!std::getline(std::cin, line)) {
            break;
        }
        if (!line.empty()) {
            std::cout << "\nCOMMAND> " << line << "\n";
            std::cout.flush();
        }
        auto tokens = tokenize(line);
        if (!processCommand(engine, tokens, nextAutoId)) {
            std::cout << "Exiting trading terminal. Goodbye!\n";
            std::cout.flush();
            break;
        }

        // Flush after every command so viewer.txt stays current while the
        // interactive session is running.
        std::cout.flush();
        std::cerr.flush();
    }
}

int runEngineCli(int argc, char* argv[]) {
    // 1. Initialize matching engine configuration
    OrderBookConfig config(1, 100000, 1000000);
    MatchingEngine engine(config);
    OrderId nextAutoId = 1001;

    // 2. Wire up execution callbacks
    engine.setTradeListener([](const ExecutionReport& report) {
        std::cout << ">> [TRADE EXECUTION] Match Price: " << report.matchPrice
                  << " | Match Qty: " << report.matchQuantity
                  << " | Maker Order #" << report.makerOrderId
                  << " (Remaining: " << report.makerRemainingQuantity << ")"
                  << " | Taker Order #" << report.takerOrderId
                  << " (Remaining: " << report.takerRemainingQuantity << ")\n";
    });

    engine.setCancelListener([](OrderId id) {
        std::cout << ">> [ORDER CANCELED] Order #" << id << " successfully removed from book.\n";
    });

    engine.setRejectListener([](OrderId id, RejectReason reason) {
        std::cout << ">> [ORDER REJECTED] Order #" << id << " rejected: " << rejectReasonToString(reason) << "\n";
    });

    // 3. Inspect argc and argv
    if (argc <= 1) {
        // No arguments passed -> Launch Interactive Trading Shell
        runInteractiveSession(engine, nextAutoId);
        return 0;
    }

    std::string firstArg = toLower(argv[1]);

    if (firstArg == "-i" || firstArg == "--interactive" || firstArg == "interactive") {
        runInteractiveSession(engine, nextAutoId);
        return 0;
    }

    if (firstArg == "demo") {
        runDemo(engine);
        return 0;
    }

    if (firstArg == "-h" || firstArg == "--help" || firstArg == "help") {
        printHelp();
        return 0;
    }

    // 4. Command line arguments execution
    // Collect all arguments from argv[1..argc-1] into tokens
    std::vector<std::string> cliTokens;
    for (int i = 1; i < argc; ++i) {
        cliTokens.push_back(argv[i]);
    }

    // Execute the command passed via arguments
    processCommand(engine, cliTokens, nextAutoId);

    // If an order operation was performed, show the resulting book depth
    if (firstArg == "limit" || firstArg == "market" || firstArg == "cancel" || firstArg == "modify") {
        printOrderBookDepth(engine.book());
    }

    return 0;
}


// These functions are implemented in their respective source files.
int runUnitTests();
int runDifferentialTest(int argc, char** argv);
int runBenchmarks();

static void printProgramHelp(const char* programName) {
    std::cout << "Limit Order Book & Matching Engine\n\n"
              << "Usage:\n"
              << "  " << programName << "                 Start interactive trading mode\n"
              << "  " << programName << " demo            Run the guided demo\n"
              << "  " << programName << " test            Run unit tests\n"
              << "  " << programName << " diff [ops] [seed]  Run differential tests (default: 25000 42)\n"
              << "  " << programName << " bench           Run performance benchmarks\n"
              << "  " << programName << " help            Show this help\n\n"
              << "Trading commands can also be passed directly, for example:\n"
              << "  " << programName << " limit buy 100 50 101\n"
              << "  " << programName << " market sell 20 201\n"
              << "  " << programName << " book 10\n"
              << "  " << programName << " cancel 101\n\n"
              << "To run multiple trading commands in one session, start the program with no arguments.\n";
}

static int runProgram(int argc, char* argv[]) {
    if (argc <= 1) {
        return runEngineCli(argc, argv);
    }

    const std::string mode = toLower(argv[1]);

    if (mode == "test") {
        return runUnitTests();
    }

    if (mode == "diff") {
        if (argc == 2) {
            char defaultOperations[] = "25000";
            char defaultSeed[] = "42";
            char* defaultArgs[] = { defaultOperations, defaultSeed };
            return runDifferentialTest(2, defaultArgs);
        }
        return runDifferentialTest(argc - 2, argv + 2);
    }

    if (mode == "bench") {
        return runBenchmarks();
    }

    if (mode == "--help" || mode == "-h" || mode == "help") {
        printProgramHelp(argv[0]);
        return 0;
    }

    // If the first argument isn't a program mode, treat all arguments as a
    // normal trading command, such as: lob.exe limit buy 100 50 101.
    return runEngineCli(argc, argv);
}


int main(int argc, char* argv[]) {
    ViewerOutputRedirect output;
    if (!output.isOpen()) {
        writeToTerminal("[ERROR] Could not create viewer.txt in the current folder.\n");
        return 1;
    }

    writeToTerminal("Program output is going to viewer.txt.\n");
    if (argc <= 1 || (argc >= 2 &&
        (toLower(argv[1]) == "interactive" || toLower(argv[1]) == "-i" ||
         toLower(argv[1]) == "--interactive"))) {
        writeToTerminal("Enter trading commands here; responses will appear in viewer.txt.\n\n");
    }

    const int result = runProgram(argc, argv);
    output.flush();

    if (argc <= 1 || (argc >= 2 &&
        (toLower(argv[1]) == "interactive" || toLower(argv[1]) == "-i" ||
         toLower(argv[1]) == "--interactive"))) {
        writeToTerminal("\nTrading session ended. Final output is in viewer.txt.\n");
    } else {
        writeToTerminal("Finished. Results are in viewer.txt.\n");
    }
    return result;
}
