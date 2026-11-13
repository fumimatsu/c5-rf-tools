// Portable algorithm/decoder checks; this does not simulate hardware or RF.
#include "decoder_self_test.h"
#include <cstdio>
void rf_self_test();
int main() {
    c5_decoder_self_test();
    rf_self_test();
    std::puts("HOST_SELF_TESTS_PASSED decoder RF transport order wrap recovery reference-preservation");
}
