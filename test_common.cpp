#include <cmath>
#include <cstdio>
#include "common.h"

// Check that always runs, regardless of NDEBUG
#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
        return; \
    } \
} while (0)

static void test_generate_sizes_count() {
    auto sizes = generate_sizes();
    CHECK(sizes.size() == 21);
    printf("PASS: test_generate_sizes_count (got %zu sizes)\n", sizes.size());
}

static void test_generate_sizes_powers_of_two() {
    auto sizes = generate_sizes();
    for (size_t i = 0; i < sizes.size(); i++) {
        CHECK(sizes[i] == (size_t{1} << i));
        CHECK((sizes[i] & (sizes[i] - 1)) == 0);
    }
    printf("PASS: test_generate_sizes_powers_of_two\n");
}

static void test_generate_sizes_range() {
    auto sizes = generate_sizes();
    CHECK(sizes.front() == 1);
    CHECK(sizes.back() == 1048576);
    printf("PASS: test_generate_sizes_range\n");
}

static void test_compute_throughput_bps() {
    auto r = compute_throughput(1, 1, 1.0);
    CHECK(r.unit == "bps");
    CHECK(std::fabs(r.value - 8.0) < 0.01);
    printf("PASS: test_compute_throughput_bps (%.2f %s)\n", r.value, r.unit.c_str());
}

static void test_compute_throughput_Kbps() {
    auto r = compute_throughput(1000, 1, 1.0);
    CHECK(r.unit == "Kbps");
    CHECK(std::fabs(r.value - 8.0) < 0.01);
    printf("PASS: test_compute_throughput_Kbps (%.2f %s)\n", r.value, r.unit.c_str());
}

static void test_compute_throughput_Mbps() {
    auto r = compute_throughput(125000, 1, 1.0);
    CHECK(r.unit == "Mbps");
    CHECK(std::fabs(r.value - 1.0) < 0.01);
    printf("PASS: test_compute_throughput_Mbps (%.2f %s)\n", r.value, r.unit.c_str());
}

static void test_compute_throughput_Gbps() {
    auto r = compute_throughput(125000000, 1, 1.0);
    CHECK(r.unit == "Gbps");
    CHECK(std::fabs(r.value - 1.0) < 0.01);
    printf("PASS: test_compute_throughput_Gbps (%.2f %s)\n", r.value, r.unit.c_str());
}

static void test_compute_throughput_boundary_Mbps_to_Gbps() {
    auto r = compute_throughput(12500000, 1, 1.0);
    CHECK(r.unit == "Mbps");
    CHECK(std::fabs(r.value - 100.0) < 0.01);
    printf("PASS: test_compute_throughput_boundary_Mbps_to_Gbps (%.2f %s)\n", r.value, r.unit.c_str());
}

static void test_compute_throughput_zero_elapsed() {
    auto r = compute_throughput(1000, 1, 0.0);
    CHECK(r.unit == "bps");
    CHECK(std::fabs(r.value - 0.0) < 0.01);
    printf("PASS: test_compute_throughput_zero_elapsed (%.2f %s)\n", r.value, r.unit.c_str());
}

int main() {
    test_generate_sizes_count();
    test_generate_sizes_powers_of_two();
    test_generate_sizes_range();
    test_compute_throughput_bps();
    test_compute_throughput_Kbps();
    test_compute_throughput_Mbps();
    test_compute_throughput_Gbps();
    test_compute_throughput_boundary_Mbps_to_Gbps();
    test_compute_throughput_zero_elapsed();
    printf("All tests passed.\n");
    return 0;
}
