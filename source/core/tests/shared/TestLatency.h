//
// created by @sfarny
//

#ifndef QB_TESTLATENCY_H
#define QB_TESTLATENCY_H

#include <iostream>
#include <chrono>
#include <iomanip>
#include <array>

namespace pg
{
    template <std::uint64_t TMaxDurationNs, std::uint64_t TBucketCount = 1000000>
    class latency
    {
    private:
        size_t count { 0 };
        std::chrono::nanoseconds bucketDuration{TMaxDurationNs / TBucketCount};
        std::array<size_t, TBucketCount> buckets{0};
        size_t outOufBoundCount { 0 };
        std::chrono::nanoseconds maxDuration;

    public:
        template <typename T>
        void add(T duration)
        {
            count++;

            auto bucketIndex = (std::uint64_t)(duration / bucketDuration);
            if (bucketIndex <= TBucketCount)
            {
                buckets[bucketIndex]++;
                return;
            }

            outOufBoundCount++;
            if (duration > maxDuration)
                maxDuration = duration;
        }

        template <typename O, typename TRatio = std::chrono::microseconds>
        void generate(O& output, const char* unit)
        {
//            output  << std::setw(20) << "duration"
//                    << std::setw(21) << "percentile"
//                    << std::setw(20) << "count"
//                    << std::endl;

            size_t cum = 0;
            size_t q50 = 0;
            size_t q99 = 0;
            size_t q999 = 0;
            double mean = 0;

            for(size_t i = 0; i < TBucketCount; i++)
            {
                auto current = buckets[i];
                accumulate_and_print(output, unit, cum, mean, q50, q99, q999, current, std::chrono::duration_cast<TRatio>((i+1) * bucketDuration));
            }

            accumulate_and_print(output, unit, cum, mean, q50, q99, q999, outOufBoundCount, std::chrono::duration_cast<TRatio>(maxDuration));

            output  << "# Mean  " << std::setw(10) << std::chrono::duration_cast<TRatio>(mean * bucketDuration).count() << unit << std::setw(10)
                    << "# Q50   " <<  std::setw(10) << std::chrono::duration_cast<TRatio>(q50 * bucketDuration).count() << unit << std::setw(10)
                    << "# Q99   " <<  std::setw(10) << std::chrono::duration_cast<TRatio>(q99 * bucketDuration).count() << unit << std::setw(10)
                    << "# Q99.9 " <<  std::setw(10) << std::chrono::duration_cast<TRatio>(q999 * bucketDuration).count() << unit << std::endl;
        }

        template <typename O, typename T>
        void accumulate_and_print(O&, const char*, size_t& cum, double& mean, size_t& q50, size_t& q99, size_t& q999, size_t current, T duration)
        {
            if (current == 0)
                return;

            cum += current;

            auto percentile = (double)cum / count * 100.0;
//            output  << std::setw(20) << duration.count() << unit
//                    << std::setw(20) << percentile << "%"
//                    << std::setw(20) << current
//                    << std::endl;

            mean = (mean * (cum-current) + current * (duration / bucketDuration)) / cum;

            if (q50 == 0 && percentile > 50.0)
                q50 = duration / bucketDuration;
            if (q99 == 0 && percentile > 99.0)
                q99 = duration / bucketDuration;
            if (q999 == 0 && percentile > 99.9)
                q999 = duration / bucketDuration;
        }
    };
}

#endif //QB_TESTLATENCY_H
