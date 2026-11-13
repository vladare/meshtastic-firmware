#include "modules/BoardFinder/boardfinder_protocol.h"
#include <unity.h>
#include <cstring>

using namespace boardfinder;

void test_encode_decode_get_config()
{
    Message req{};
    req.protocol_version = PROTOCOL_VERSION;
    req.request_id = 42;
    req.command = Command::GET_CONFIG;

    uint8_t buf[MAX_MESSAGE_BYTES];
    size_t len = encodeMessage(req, buf, sizeof(buf));
    TEST_ASSERT_TRUE(len > 0);

    Message out{};
    TEST_ASSERT_TRUE(decodeMessage(buf, len, out));
    TEST_ASSERT_EQUAL_UINT32(PROTOCOL_VERSION, out.protocol_version);
    TEST_ASSERT_EQUAL_UINT32(42, out.request_id);
    TEST_ASSERT_EQUAL(static_cast<int>(Command::GET_CONFIG), static_cast<int>(out.command));
    TEST_ASSERT_FALSE(out.has_ble_burst_enabled);
}

void test_encode_decode_set_config_on()
{
    Message req{};
    req.protocol_version = PROTOCOL_VERSION;
    req.request_id = 7;
    req.command = Command::SET_CONFIG;
    req.ble_burst_enabled = true;
    req.has_ble_burst_enabled = true;

    uint8_t buf[MAX_MESSAGE_BYTES];
    size_t len = encodeMessage(req, buf, sizeof(buf));
    TEST_ASSERT_TRUE(len > 0);

    Message out{};
    TEST_ASSERT_TRUE(decodeMessage(buf, len, out));
    TEST_ASSERT_EQUAL(static_cast<int>(Command::SET_CONFIG), static_cast<int>(out.command));
    TEST_ASSERT_TRUE(out.has_ble_burst_enabled);
    TEST_ASSERT_TRUE(out.ble_burst_enabled);
}

void test_encode_decode_config_response_off()
{
    Message resp{};
    resp.protocol_version = PROTOCOL_VERSION;
    resp.request_id = 9;
    resp.command = Command::CONFIG_RESPONSE;
    resp.ble_burst_enabled = false;
    resp.has_ble_burst_enabled = true;
    resp.status = Status::OK;

    uint8_t buf[MAX_MESSAGE_BYTES];
    size_t len = encodeMessage(resp, buf, sizeof(buf));
    TEST_ASSERT_TRUE(len > 0);

    Message out{};
    TEST_ASSERT_TRUE(decodeMessage(buf, len, out));
    TEST_ASSERT_EQUAL(static_cast<int>(Command::CONFIG_RESPONSE), static_cast<int>(out.command));
    TEST_ASSERT_TRUE(out.has_ble_burst_enabled);
    TEST_ASSERT_FALSE(out.ble_burst_enabled);
    TEST_ASSERT_EQUAL(static_cast<int>(Status::OK), static_cast<int>(out.status));
}

void test_malformed_empty()
{
    Message out{};
    TEST_ASSERT_FALSE(decodeMessage(nullptr, 0, out));
    uint8_t empty = 0;
    TEST_ASSERT_FALSE(decodeMessage(&empty, 0, out));
}

void test_idempotent_set_encoding()
{
    Message a{};
    a.protocol_version = PROTOCOL_VERSION;
    a.request_id = 1;
    a.command = Command::SET_CONFIG;
    a.ble_burst_enabled = true;
    a.has_ble_burst_enabled = true;

    Message b = a;
    uint8_t bufA[MAX_MESSAGE_BYTES];
    uint8_t bufB[MAX_MESSAGE_BYTES];
    size_t lenA = encodeMessage(a, bufA, sizeof(bufA));
    size_t lenB = encodeMessage(b, bufB, sizeof(bufB));
    TEST_ASSERT_EQUAL(lenA, lenB);
    TEST_ASSERT_EQUAL_MEMORY(bufA, bufB, lenA);
}

void setup()
{
    UNITY_BEGIN();
    RUN_TEST(test_encode_decode_get_config);
    RUN_TEST(test_encode_decode_set_config_on);
    RUN_TEST(test_encode_decode_config_response_off);
    RUN_TEST(test_malformed_empty);
    RUN_TEST(test_idempotent_set_encoding);
    UNITY_END();
}

void loop() {}
