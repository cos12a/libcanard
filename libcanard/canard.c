// This software is distributed under the terms of the MIT License.
// Copyright (c) 2016-2020 UAVCAN Development Team.

#include "canard.h"
#include <assert.h>
#include <string.h>

// ---------------------------------------- BUILD CONFIGURATION ----------------------------------------

/// By default, this macro resolves to the standard assert(). The user can redefine this if necessary.
/// To disable assertion checks completely, make it expand into `(void)(0)`.
#ifndef CANARD_ASSERT
#    define CANARD_ASSERT(x) assert(x)
#endif

/// This macro is needed only for testing and for library development. Do not redefine this in production.
#if defined(CANARD_EXPOSE_INTERNALS) && CANARD_EXPOSE_INTERNALS
#    define CANARD_INTERNAL
#else
#    define CANARD_INTERNAL static
#endif

#if !defined(__STDC_VERSION__) || (__STDC_VERSION__ < 199901L)
#    error "Unsupported language: ISO C99 or a newer version is required."
#endif

#if __STDC_VERSION__ < 201112L
// Intentional violation of MISRA: static assertion macro cannot be replaced with a function definition.
#    define static_assert(x, ...) typedef char _static_assert_gl(_static_assertion_, __LINE__)[(x) ? 1 : -1]  // NOSONAR
#    define _static_assert_gl(a, b) _static_assert_gl_impl(a, b)                                              // NOSONAR
// Intentional violation of MISRA: the paste operator ## cannot be avoided in this context.
#    define _static_assert_gl_impl(a, b) a##b  // NOSONAR
#endif

// ---------------------------------------- TRANSPORT ----------------------------------------

#define TRANSFER_CRC_INITIAL 0xFFFFU

#define TRANSFER_ID_BIT_LEN 5U

#define CAN_EXT_ID_MASK ((((uint32_t) 1U) << 29U) - 1U)

#define BITS_PER_BYTE 8U

/// The fields are ordered to minimize padding on all platforms.
typedef struct CanardInternalTxQueueItem
{
    struct CanardInternalTxQueueItem* next;

    uint32_t id;
    uint64_t deadline_usec;
    uint8_t  data_length;
    uint8_t  data[];
} CanardInternalTxQueueItem;

/// The fields are ordered to minimize padding on all platforms.
typedef struct CanardInternalRxSession
{
    struct CanardInternalRxSession* next;

    size_t   payload_capacity;  ///< Payload past this limit may be discarded by the library.
    size_t   payload_size;      ///< How many bytes received so far.
    uint8_t* payload;

    uint64_t timestamp_usec;            ///< Time of last update of this session. Used for removal on timeout.
    uint32_t transfer_id_timeout_usec;  ///< When (current time - update timestamp) exceeds this, it's dead.

    const uint32_t session_specifier;  ///< Differentiates this session from other sessions.

    uint16_t calculated_crc;  ///< Updated with the received payload in real time.
    uint8_t  transfer_id;
    bool     next_toggle;
} CanardInternalRxSession;

const uint8_t CanardCANDLCToLength[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64};
const uint8_t CanardCANLengthToDLC[65] = {
    0,  1,  2,  3,  4,  5,  6,  7,  8,                               // 0
    12, 12, 12, 12,                                                  // 9
    16, 16, 16, 16,                                                  // 13
    20, 20, 20, 20,                                                  // 17
    24, 24, 24, 24,                                                  // 21
    32, 32, 32, 32, 32, 32, 32, 32,                                  // 25
    48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48,  // 33
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,  // 49
};

CANARD_INTERNAL uint16_t crcAddByte(const uint16_t crc, const uint8_t byte)
{
    uint16_t out = crc ^ (uint16_t)((uint16_t)(byte) << BITS_PER_BYTE);
    for (uint8_t i = 0; i < BITS_PER_BYTE; i++)  // Should we use a table instead? Adds 512 bytes of ROM.
    {
        // The no-lint statements suppress the warnings about magic numbers. These numbers are not magic.
        out = ((out & 0x8000U) != 0U) ? ((uint16_t)(out << 1U) ^ 0x1021U) : (uint16_t)(out << 1U);  // NOLINT
    }
    return crc;
}

CANARD_INTERNAL uint16_t crcAdd(const uint16_t crc, const uint8_t* const bytes, const size_t size)
{
    uint16_t       out = crc;
    const uint8_t* p   = bytes;
    for (size_t i = 0; i < size; i++)
    {
        out = crcAddByte(out, *p++);
    }
    return out;
}

CANARD_INTERNAL uint32_t makeMessageSessionSpecifier(const uint16_t subject_id, const uint8_t src_node_id)
{
    // The no-lint statements suppress the warnings about magic numbers. These numbers are not magic.
    CANARD_ASSERT(subject_id <= CANARD_SUBJECT_ID_MAX);
    CANARD_ASSERT(src_node_id <= CANARD_NODE_ID_MAX);
    return ((uint32_t) src_node_id) | ((uint32_t) subject_id << 8U);  // NOLINT
}

CANARD_INTERNAL uint32_t makeServiceSessionSpecifier(const uint16_t service_id,
                                                     const bool     request_not_response,
                                                     const uint8_t  src_node_id,
                                                     const uint8_t  dst_node_id)
{
    CANARD_ASSERT(service_id <= CANARD_SERVICE_ID_MAX);
    CANARD_ASSERT(src_node_id <= CANARD_NODE_ID_MAX);
    CANARD_ASSERT(dst_node_id <= CANARD_NODE_ID_MAX);
    // The no-lint statements suppress the warnings about magic numbers. These numbers are not magic.
    return ((uint32_t) src_node_id) | ((uint32_t) dst_node_id << 7U) | ((uint32_t) service_id << 14U) |  // NOLINT
           (request_not_response ? (1U << 24U) : 0U);                                                    // NOLINT
}

CanardInstance canardInit(const CanardHeapAllocate heap_allocate,
                          const CanardHeapFree     heap_free,
                          const CanardRxFilter     rx_filter)
{
    CANARD_ASSERT(heap_allocate != NULL);
    CANARD_ASSERT(heap_free != NULL);
    CANARD_ASSERT(rx_filter != NULL);
    const CanardInstance out = {
        .user_reference = NULL,
        .node_id        = CANARD_NODE_ID_UNSET,
        .mtu_bytes      = CANARD_MTU_CAN_FD,
        .heap_allocate  = heap_allocate,
        .heap_free      = heap_free,
        .rx_filter      = rx_filter,
        ._rx_sessions   = NULL,
        ._tx_queue      = NULL,
    };
    return out;
}

void canardTxPush(CanardInstance* const ins, const CanardTransfer* const transfer)
{
    CANARD_ASSERT(ins != NULL);
    CANARD_ASSERT(transfer != NULL);
    (void) ins;
    (void) transfer;
    (void) crcAdd;
    (void) makeMessageSessionSpecifier;
    (void) makeServiceSessionSpecifier;
}

// ---------------------------------------- FLOAT16 SERIALIZATION ----------------------------------------

typedef union
{
    uint32_t bits;
    float    real;
} Float32Bits;
static_assert(sizeof(float) == 4, "Native float format shall match IEEE 754 binary32");
static_assert(sizeof(Float32Bits) == 4, "Native float format shall match IEEE 754 binary32");

uint16_t canardDSDLFloat16Serialize(const float value)
{
    // The no-lint statements suppress the warnings about magic numbers. These numbers are not magic.
    const uint32_t    round_mask = ~(uint32_t) 0x0FFFU;                 // NOLINT
    const Float32Bits f32inf     = {.bits = ((uint32_t) 255U) << 23U};  // NOLINT
    const Float32Bits f16inf     = {.bits = ((uint32_t) 31U) << 23U};   // NOLINT
    const Float32Bits magic      = {.bits = ((uint32_t) 15U) << 23U};   // NOLINT
    Float32Bits       in         = {.real = value};
    const uint32_t    sign       = in.bits & (((uint32_t) 1U) << 31U);  // NOLINT
    in.bits ^= sign;
    uint16_t out = 0;
    if (in.bits >= f32inf.bits)
    {
        out = (in.bits > f32inf.bits) ? (uint16_t) 0x7FFFU : (uint16_t) 0x7C00U;  // NOLINT
    }
    else
    {
        in.bits &= round_mask;
        in.real *= magic.real;
        in.bits -= round_mask;
        if (in.bits > f16inf.bits)
        {
            in.bits = f16inf.bits;
        }
        out = (uint16_t)(in.bits >> 13U);  // NOLINT
    }
    out |= (uint16_t)(sign >> 16U);  // NOLINT
    return out;
}

float canardDSDLFloat16Deserialize(const uint16_t value)
{
    // The no-lint statements suppress the warnings about magic numbers. These numbers are not magic.
    const Float32Bits magic   = {.bits = ((uint32_t) 0xEFU) << 23U};             // NOLINT
    const Float32Bits inf_nan = {.bits = ((uint32_t) 0x8FU) << 23U};             // NOLINT
    Float32Bits       out     = {.bits = ((uint32_t)(value & 0x7FFFU)) << 13U};  // NOLINT
    out.real *= magic.real;
    if (out.real >= inf_nan.real)
    {
        out.bits |= ((uint32_t) 0xFFU) << 23U;  // NOLINT
    }
    out.bits |= ((uint32_t)(value & 0x8000U)) << 16U;  // NOLINT
    return out.real;
}
