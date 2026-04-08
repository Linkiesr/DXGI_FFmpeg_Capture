#pragma once

#include <QString>

// 单阶段耗时统计：最小值、最大值、平均值。
struct TimingStats {
    int64_t count = 0;
    double minMs = 0.0;
    double maxMs = 0.0;
    double totalMs = 0.0;

    void addSample(double ms) {
        if (count == 0) {
            minMs = ms;
            maxMs = ms;
        } else {
            if (ms < minMs) {
                minMs = ms;
            }
            if (ms > maxMs) {
                maxMs = ms;
            }
        }

        totalMs += ms;
        ++count;
    }

    double avgMs() const {
        return count > 0 ? (totalMs / static_cast<double>(count)) : 0.0;
    }

    QString summary(const QString& stageName) const {
        return QString("%1: count=%2, min=%3 ms, max=%4 ms, avg=%5 ms")
            .arg(stageName)
            .arg(count)
            .arg(minMs, 0, 'f', 3)
            .arg(maxMs, 0, 'f', 3)
            .arg(avgMs(), 0, 'f', 3);
    }
};
