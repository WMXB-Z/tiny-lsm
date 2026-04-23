#pragma once
#include <set>

// 把一个“连续递增前缀”压缩成只保留“最大值”，从而减少集合大小。
// 例如{1, 2, 3, 4, 7, 9}，其中1-4是连续的，故直接压缩为{4,7,9}
template <class T>
std::set<T> compressSet(const std::set<T>& s) {
    if (s.empty()) return std::set<T>();

    std::set<T> result;
    auto it = s.begin();
    T last = *it;
    ++it;
    while (it != s.end()) {
        if (*it == last + 1) {
            last = *it;
        } else {
            break;
        }
        ++it;
    }
    result.insert(last);
    while (it != s.end()) {
        result.insert(*it);
        ++it;
    }
    return result;
}