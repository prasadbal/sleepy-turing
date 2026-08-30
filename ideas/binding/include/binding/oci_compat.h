#pragma once

// Chooses the Oracle Call Interface headers to build against: the real
// Oracle client <oci.h> when it's available on the include path, otherwise
// this repo's mock (binding/oci_mock.h). The two are never included
// together -- both define the same global OCI_* symbols and typedefs, and
// including both would collide.
#if __has_include(<oci.h>)
    #include <oci.h>
    #define BINDING_HAS_REAL_OCI 1
#else
    #include "binding/oci_mock.h"
    #define BINDING_HAS_REAL_OCI 0
#endif
