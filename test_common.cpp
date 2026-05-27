#include <cassert>
#include <cstdio>
#include <cmath>
#include "common.h"

void test_generate_sizes_count() {
    auto sizes = generate_sizes();
    assert(sizes.size() == 21);
    printf("PASS: test_generate_sizes_count (got %zu sizes)\n", sizes.size());
}

void test_generate_sizes_powers_of_two() {
    auto sizes = generate_sizes();
    for (size_t i = 0; i < sizes.size(); i++) {
        assert(sizes[i] == (size_t{1} << i));
        assert((sizes[i] & (sizes[i] - 1)) == 0);
    }
    printf("PASS: test_generate_sizes_powers_of_two\n");
}

void test_generate_sizes_range() {
    auto sizes = generate_sizes();
    assert(sizes.front() == 1);
    assert(sizes.back() == 1048576);
    printf("PASS: test_generate_sizes_range\n");
}

void test_compute_throughput_bps() {
    // 1 byte in 1 second = 8 bits/sec
    auto r = compute_throughput(1, 1, 1.0);
    assert(r.unit == "bps");
    assert(std::fabs(r.value - 8.0) < 0.01);
    printf("PASS: test_compute_throughput_bps (%.2f %s)\n", r.value, r.unit.c_str());
}

void test_compute_throughput_Kbps() {
    // 1000 bytes in 1 second = 8000 bps = 8 Kbps
    auto r = compute_throughput(1000, 1, 1.0);
    assert(r.unit == "Kbps");
    assert(std::fabs(r.value - 8.0) < 0.01);
    printf("PASS: test_compute_throughput_Kbps (%.2f %s)\n", r.value, r.unit.c_str());
}

void test_compute_throughput_Mbps() {
    // 125000 bytes in 1 second = 1 Mbps
    auto r = compute_throughput(125000, 1, 1.0);
    assert(r.unit == "Mbps");
    assert(std::fabs(r.value - 1.0) < 0.01);
    printf("PASS: test_compute_throughput_Mbps (%.2f %s)\n", r.value, r.unit.c_str());
}

void test_compute_throughput_Gbps() {
    // 125000000 bytes in 1 second = 1 Gbps
    auto r = compute_throughput(125000000, 1, 1.0);
    assert(r.unit == "Gbps");
    assert(std::fabs(r.value - 1.0) < 0.01);
    printf("PASS: test_compute_throughput_Gbps (%.2f %s)\n", r.value, r.unit.c_str());
}

void test_compute_throughput_boundary_Mbps_to_Gbps() {
    // 12500000 bytes (12.5MB) in 1 sec = 100 Mbps
    auto r = compute_throughput(12500000, 1, 1.0);
    assert(r.unit == "Mbps");
    assert(std::fabs(r.value - 100.0) < 0.01);
    printf("PASS: test_compute_throughput_boundary_Mbps_to_Gbps (%.2f %s)\n", r.value, r.unit.c_str());
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
    printf("All tests passed.\n");
    return 0;
}
