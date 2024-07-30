#include <iostream>
#include "klangas/sineosc.h"

inline void test_klangas()
{
    double sr = 44100.0;
    auto as = std::make_unique<AdditiveSynth>();
    as->prepare(sr);
}

int main()
{
    test_klangas();
}
