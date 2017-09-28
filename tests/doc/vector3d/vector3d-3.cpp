// Chemfiles, a modern library for chemistry file reading and writing
// Copyright (C) Guillaume Fraux and contributors -- BSD license
#include <chemfiles.hpp>
using namespace chemfiles;

int main() {
    // [example]
    auto u = Vector3D(1.5, 2.0, -3.0);
    assert(u[0] == 1.5);
    assert(u[1] == 2.0);
    assert(u[2] == -3.0);
    // [example]
    return 0;
}
