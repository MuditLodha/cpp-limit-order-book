#pragma once

#include <vector>
#include <numeric>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <iomanip>
#include <string>
#include <chrono>
#include <limits>

#if defined(_MSC_VER) || defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif

namespace matching_engine {

/**
 * @brief High-precision timer using raw CPU cycle counters (RDTSC).
 */
class HighResolutionTimer {
public:
    static inline uint64_t rdtsc() noexcept {
#if defined(__x86_64__) || defined(__i386__)
        return __rdtsc();
#elif defined(_MSC_VER)
        return __rdtsc();
#else
        return static_cast<uint64_t>(
            std::chrono::high_resolution_clock::now().time_since_epoch().count());
#endif
    }

    static inline uint64_t nowNanos() noexcept {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::high_resolution_clock::now().time_since_epoch()).count());
    }

    /**
     * @brief Calibrates the CPU clock to translate raw cycles into exact nanoseconds.
     */
    static double cyclesPerNanosecond() noexcept {
        static double ghz = []() {
            using namespace std::chrono;
            const auto startChrono = high_resolution_clock::now();
            const uint64_t startRdtsc = rdtsc();

            // Calibrate over a 25ms window
            while (duration_cast<milliseconds>(high_resolution_clock::now() - startChrono).count() < 25) {
                #if defined(__x86_64__) || defined(_M_X64)
                _mm_pause();
                #endif
            }

            const auto endChrono = high_resolution_clock::now();
            const uint64_t endRdtsc = rdtsc();

            const double nanos = static_cast<double>(duration_cast<nanoseconds>(endChrono - startChrono).count());
            const double cycles = static_cast<double>(endRdtsc - startRdtsc);
            return (cycles / nanos);
        }();
        return ghz;
    }

    static uint64_t rdtscOverhead() noexcept {
        static uint64_t overhead = []() {
            uint64_t minDiff = std::numeric_limits<uint64_t>::max();
            for (int i = 0; i < 500; ++i) {
                uint64_t t0 = rdtsc();
                uint64_t t1 = rdtsc();
                uint64_t diff = t1 - t0;
                if (diff < minDiff) minDiff = diff;
            }
            return minDiff;
        }();
        return overhead;
    }
};

/**
 * @brief Latency recorder and statistical percentile calculator.
 */
class LatencyHistogram {
public:
    explicit LatencyHistogram(size_t expectedSamples = 1000000) {
        samples_.reserve(expectedSamples);
    }

    void record(uint64_t latency) {
        samples_.push_back(latency);
    }

    void reset() {
        samples_.clear();
    }

    size_t count() const {
        return samples_.size();
    }

    struct Summary {
        size_t count{0};
        double min{0.0};
        double mean{0.0};
        double stddev{0.0};
        double p50{0.0};
        double p90{0.0};
        double p99{0.0};
        double p99_9{0.0};
        double p99_99{0.0};
        double max{0.0};
    };

    Summary compute() {
        if (samples_.empty()) return {};

        std::sort(samples_.begin(), samples_.end());

        Summary s;
        s.count = samples_.size();
        s.min = static_cast<double>(samples_.front());
        s.max = static_cast<double>(samples_.back());

        const double sum = std::accumulate(samples_.begin(), samples_.end(), 0.0);
        s.mean = sum / static_cast<double>(s.count);

        double varSum = 0.0;
        for (auto val : samples_) {
            const double diff = static_cast<double>(val) - s.mean;
            varSum += diff * diff;
        }
        s.stddev = std::sqrt(varSum / static_cast<double>(s.count));

        s.p50 = percentile(0.50);
        s.p90 = percentile(0.90);
        s.p99 = percentile(0.99);
        s.p99_9 = percentile(0.999);
        s.p99_99 = percentile(0.9999);

        return s;
    }

    void printSummary(const std::string& testName, const std::string& unit = "cycles", double scale = 1.0) {
        const Summary s = compute();
        std::cout << "\n=======================================================\n";
        std::cout << " Benchmark: " << testName << "\n";
        std::cout << " Samples  : " << s.count << "\n";
        std::cout << "-------------------------------------------------------\n";
        std::cout << std::fixed << std::setprecision(2);
        std::cout << " Min      : " << std::setw(10) << (s.min * scale) << " " << unit << "\n";
        std::cout << " Mean     : " << std::setw(10) << (s.mean * scale) << " " << unit << "\n";
        std::cout << " StdDev   : " << std::setw(10) << (s.stddev * scale) << " " << unit << "\n";
        std::cout << " P50      : " << std::setw(10) << (s.p50 * scale) << " " << unit << "\n";
        std::cout << " P90      : " << std::setw(10) << (s.p90 * scale) << " " << unit << "\n";
        std::cout << " P99      : " << std::setw(10) << (s.p99 * scale) << " " << unit << "\n";
        std::cout << " P99.9    : " << std::setw(10) << (s.p99_9 * scale) << " " << unit << "\n";
        std::cout << " P99.99   : " << std::setw(10) << (s.p99_99 * scale) << " " << unit << "\n";
        std::cout << " Max      : " << std::setw(10) << (s.max * scale) << " " << unit << "\n";
        std::cout << "=======================================================\n";
    }

private:
    double percentile(double p) const {
        if (samples_.empty()) return 0.0;
        const double index = p * static_cast<double>(samples_.size() - 1);
        const size_t lower = static_cast<size_t>(std::floor(index));
        const size_t upper = static_cast<size_t>(std::ceil(index));
        if (lower == upper) return static_cast<double>(samples_[lower]);
        const double weight = index - static_cast<double>(lower);
        return static_cast<double>(samples_[lower]) * (1.0 - weight) +
               static_cast<double>(samples_[upper]) * weight;
    }

    std::vector<uint64_t> samples_;
};

} // namespace matching_engine
