#include <iostream>
#include <vector>
#include <algorithm>
#include <random>
#include <chrono>
#include <functional>
#include <numeric>
#include <map>
#include <sstream>

template <typename T>
class QuantumHeap {
    std::vector<T> nodes;
    std::function<bool(T, T)> comparator;

public:
    QuantumHeap(std::function<bool(T, T)> cmp) : comparator(cmp) {}

    void push(T val) {
        nodes.push_back(val);
        std::push_heap(nodes.begin(), nodes.end(), comparator);
    }

    T pop() {
        std::pop_heap(nodes.begin(), nodes.end(), comparator);
        T top = nodes.back();
        nodes.pop_back();
        return top;
    }

    bool empty() const { return nodes.empty(); }
    size_t size() const { return nodes.size(); }
};

std::string encodeRunLength(const std::string& input) {
    if (input.empty()) return "";
    std::ostringstream out;
    char cur = input[0];
    int count = 1;
    for (size_t i = 1; i < input.size(); ++i) {
        if (input[i] == cur) {
            ++count;
        } else {
            out << count << cur;
            cur = input[i];
            count = 1;
        }
    }
    out << count << cur;
    return out.str();
}

std::vector<int> sieveOfEratosthenes(int limit) {
    std::vector<bool> is_prime(limit + 1, true);
    is_prime[0] = is_prime[1] = false;
    for (int i = 2; i * i <= limit; ++i)
        if (is_prime[i])
            for (int j = i * i; j <= limit; j += i)
                is_prime[j] = false;
    std::vector<int> primes;
    for (int i = 2; i <= limit; ++i)
        if (is_prime[i]) primes.push_back(i);
    return primes;
}

struct Point {
    double x, y;
};

double crossProduct(Point O, Point A, Point B) {
    return (A.x - O.x) * (B.y - O.y) - (A.y - O.y) * (B.x - O.x);
}

std::vector<Point> convexHull(std::vector<Point> points) {
    int n = points.size();
    if (n < 3) return points;
    std::sort(points.begin(), points.end(), [](Point a, Point b) {
        return a.x < b.x || (a.x == b.x && a.y < b.y);
    });
    std::vector<Point> hull;
    for (int i = 0; i < n; ++i) {
        while (hull.size() >= 2 && crossProduct(hull[hull.size()-2], hull.back(), points[i]) <= 0)
            hull.pop_back();
        hull.push_back(points[i]);
    }
    for (int i = n - 2, t = hull.size() + 1; i >= 0; --i) {
        while ((int)hull.size() >= t && crossProduct(hull[hull.size()-2], hull.back(), points[i]) <= 0)
            hull.pop_back();
        hull.push_back(points[i]);
    }
    hull.pop_back();
    return hull;
}

int main() {
    auto seed = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::mt19937 rng(seed);

    QuantumHeap<int> maxHeap([](int a, int b) { return a < b; });
    std::uniform_int_distribution<int> dist(1, 1000);
    for (int i = 0; i < 10; ++i) maxHeap.push(dist(rng));

    std::cout << "Max-Heap drain: ";
    while (!maxHeap.empty()) std::cout << maxHeap.pop() << " ";
    std::cout << "\n";

    std::string sample = "aaabbbccddddeeee";
    std::cout << "RLE of \"" << sample << "\": " << encodeRunLength(sample) << "\n";

    auto primes = sieveOfEratosthenes(100);
    std::cout << "Primes up to 100: ";
    for (int p : primes) std::cout << p << " ";
    std::cout << "\n";

    std::vector<Point> pts;
    std::uniform_real_distribution<double> pdist(0.0, 100.0);
    for (int i = 0; i < 20; ++i) pts.push_back({pdist(rng), pdist(rng)});
    auto hull = convexHull(pts);
    std::cout << "Convex hull has " << hull.size() << " vertices\n";

    std::vector<int> nums(20);
    std::iota(nums.begin(), nums.end(), 1);
    std::shuffle(nums.begin(), nums.end(), rng);
    std::cout << "Shuffled: ";
    for (int n : nums) std::cout << n << " ";
    std::cout << "\n";

    std::map<int, int> freq;
    for (int n : nums) freq[n % 5]++;
    std::cout << "Mod-5 frequency: ";
    for (auto& [k, v] : freq) std::cout << k << ":" << v << " ";
    std::cout << "\n";

    return 0;
}