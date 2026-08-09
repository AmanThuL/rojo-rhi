// Exactly one TU must emit metal-cpp's Objective-C class and selector tables. MTL4 uses the
// MTL_PRIVATE_IMPLEMENTATION gate; defining these macros elsewhere violates the ODR.
#define NS_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>
