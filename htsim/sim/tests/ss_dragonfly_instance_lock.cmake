# Hash lock for the load-discrimination experiment's declared instances.
#
# The experiment's registered rows and the recorded run manifests are tied
# to these exact instance bytes; a drifted instance would silently change
# what the registered verdicts mean.  This is the machine-enforced form of
# the manifest's topology hashes (the same tie the wave-19 review demanded
# for its instances).

foreach(required_variable IN ITEMS INSTANCE_FILE EXPECTED_SHA256)
    if(NOT DEFINED ${required_variable})
        message(FATAL_ERROR "${required_variable} is required")
    endif()
endforeach()

if(NOT EXISTS "${INSTANCE_FILE}")
    message(FATAL_ERROR "the tracked instance ${INSTANCE_FILE} is missing")
endif()
file(SHA256 "${INSTANCE_FILE}" actual_sha256)
if(NOT actual_sha256 STREQUAL EXPECTED_SHA256)
    message(FATAL_ERROR
        "the tracked instance ${INSTANCE_FILE} changed: expected "
        "${EXPECTED_SHA256}, got ${actual_sha256}")
endif()
