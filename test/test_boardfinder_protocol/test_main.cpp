#include "modules/BoardFinder/boardfinder_adv.h"
#include "modules/BoardFinder/boardfinder_protocol.h"
#include "modules/BoardFinder/boardfinder_sound.h"
#include <cstring>
#include <unity.h>

using namespace boardfinder;

void test_encode_decode_get_config()
{
    Message req{};
    req.protocol_version = 1;
    req.request_id = 42;
    req.command = Command::GET_CONFIG;

    uint8_t buf[MAX_MESSAGE_BYTES];
    size_t len = encodeMessage(req, buf, sizeof(buf));
    TEST_ASSERT_TRUE(len > 0);

    Message out{};
    TEST_ASSERT_TRUE(decodeMessage(buf, len, out));
    TEST_ASSERT_EQUAL_UINT32(1, out.protocol_version);
    TEST_ASSERT_EQUAL_UINT32(42, out.request_id);
    TEST_ASSERT_EQUAL(static_cast<int>(Command::GET_CONFIG), static_cast<int>(out.command));
    TEST_ASSERT_FALSE(out.has_ble_burst_enabled);
}

void test_encode_decode_set_config_on()
{
    Message req{};
    req.protocol_version = 1;
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
    resp.protocol_version = 1;
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
    a.protocol_version = 1;
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

void test_v2_arm_and_status()
{
    Message arm{};
    arm.protocol_version = PROTOCOL_VERSION;
    arm.request_id = 11;
    arm.command = Command::ARM_BOARD;

    uint8_t buf[MAX_MESSAGE_BYTES];
    size_t len = encodeMessage(arm, buf, sizeof(buf));
    TEST_ASSERT_TRUE(len > 0);
    Message out{};
    TEST_ASSERT_TRUE(decodeMessage(buf, len, out));
    TEST_ASSERT_EQUAL(static_cast<int>(Command::ARM_BOARD), static_cast<int>(out.command));

    Message st{};
    st.protocol_version = PROTOCOL_VERSION;
    st.request_id = 12;
    st.command = Command::BOARD_STATUS_RESPONSE;
    st.device_role = DeviceRole::BOARD;
    st.has_device_role = true;
    st.rider_node_id = 0xA1B2C3D4u;
    st.has_rider_node_id = true;
    st.board_armed = true;
    st.has_board_armed = true;
    st.board_state = BoardState::MONITORING;
    st.has_board_state = true;
    st.synchronized = true;
    st.has_synchronized = true;
    st.consecutive_missed_bursts = 0;
    st.has_consecutive_missed_bursts = true;
    st.last_rider_rssi = -61;
    st.has_last_rider_rssi = true;
    st.last_rider_seen_age_seconds = 3;
    st.has_last_rider_seen_age_seconds = true;
    st.gps_tracking_active = false;
    st.has_gps_tracking_active = true;
    st.last_gps_report_status = GpsReportStatus::NONE;
    st.has_last_gps_report_status = true;
    st.status = Status::OK;

    len = encodeMessage(st, buf, sizeof(buf));
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_TRUE(decodeMessage(buf, len, out));
    TEST_ASSERT_EQUAL(static_cast<int>(DeviceRole::BOARD), static_cast<int>(out.device_role));
    TEST_ASSERT_EQUAL_UINT32(0xA1B2C3D4u, out.rider_node_id);
    TEST_ASSERT_TRUE(out.board_armed);
    TEST_ASSERT_EQUAL(static_cast<int>(BoardState::MONITORING), static_cast<int>(out.board_state));
    TEST_ASSERT_TRUE(out.synchronized);
    TEST_ASSERT_EQUAL_INT32(-61, out.last_rider_rssi);
    TEST_ASSERT_EQUAL_UINT32(3, out.last_rider_seen_age_seconds);
}

void test_parse_rider_service_data_ok()
{
    uint8_t adv[25];
    adv[0] = 2;
    adv[1] = AD_TYPE_FLAGS;
    adv[2] = 0x06;
    adv[3] = 21;
    adv[4] = AD_TYPE_SERVICE_DATA_128BIT;
    memcpy(&adv[5], kMeshServiceUuid, 16);
    const uint32_t node = 0xA1B2C3D4u;
    adv[21] = (uint8_t)node;
    adv[22] = (uint8_t)(node >> 8);
    adv[23] = (uint8_t)(node >> 16);
    adv[24] = (uint8_t)(node >> 24);

    RiderAdvMatch m{};
    TEST_ASSERT_TRUE(parseRiderServiceData(adv, sizeof(adv), m));
    TEST_ASSERT_TRUE(m.matched);
    TEST_ASSERT_EQUAL_UINT32(node, m.node_id);
}

void test_parse_wrong_uuid()
{
    uint8_t adv[25] = {};
    adv[0] = 2;
    adv[1] = AD_TYPE_FLAGS;
    adv[2] = 0x06;
    adv[3] = 21;
    adv[4] = AD_TYPE_SERVICE_DATA_128BIT;
    memset(&adv[5], 0x11, 16);
    adv[21] = 1;
    RiderAdvMatch m{};
    TEST_ASSERT_FALSE(parseRiderServiceData(adv, sizeof(adv), m));
}

void test_parse_truncated_node_id()
{
    uint8_t adv[24] = {};
    adv[0] = 2;
    adv[1] = AD_TYPE_FLAGS;
    adv[2] = 0x06;
    adv[3] = 20; // too short for uuid+4
    adv[4] = AD_TYPE_SERVICE_DATA_128BIT;
    memcpy(&adv[5], kMeshServiceUuid, 16);
    RiderAdvMatch m{};
    TEST_ASSERT_FALSE(parseRiderServiceData(adv, sizeof(adv), m));
}

void test_same_burst_grouping()
{
    TEST_ASSERT_TRUE(sameBurst(1000, 3500, 4000));
    TEST_ASSERT_FALSE(sameBurst(1000, 6000, 4000));
}

void test_sound_pattern_rider_detected()
{
    ToneStep steps[12];
    size_t n = soundPattern(BoardFinderSound::RIDER_DETECTED, steps, 12);
    TEST_ASSERT_EQUAL(5, n); // H gap H gap H
    TEST_ASSERT_EQUAL_UINT16(HIGH_TONE_HZ, steps[0].freq_hz);
    TEST_ASSERT_EQUAL_UINT16(TONE_DURATION_MS, steps[0].duration_ms);
    TEST_ASSERT_EQUAL_UINT16(0, steps[1].freq_hz);
    TEST_ASSERT_EQUAL_UINT16(TONE_GAP_MS, steps[1].duration_ms);
    TEST_ASSERT_EQUAL_UINT16(HIGH_TONE_HZ, steps[4].freq_hz);
}

void test_sound_pattern_synchronized()
{
    ToneStep steps[12];
    size_t n = soundPattern(BoardFinderSound::SYNCHRONIZED, steps, 12);
    TEST_ASSERT_EQUAL(11, n);
    TEST_ASSERT_EQUAL_UINT16(GROUP_GAP_MS, steps[5].duration_ms);
    TEST_ASSERT_EQUAL_UINT16(LOW_TONE_HZ, steps[6].freq_hz);
    TEST_ASSERT_EQUAL_UINT16(LOW_TONE_HZ, steps[10].freq_hz);
}

void test_sound_pattern_separation()
{
    ToneStep steps[12];
    size_t n = soundPattern(BoardFinderSound::SEPARATION_DETECTED, steps, 12);
    TEST_ASSERT_EQUAL(3, n);
    TEST_ASSERT_EQUAL_UINT16(LOW_TONE_HZ, steps[0].freq_hz);
    TEST_ASSERT_EQUAL_UINT16(SEPARATION_GAP_MS, steps[1].duration_ms);
    TEST_ASSERT_EQUAL_UINT16(LOW_TONE_HZ, steps[2].freq_hz);
}

void test_sound_scheduler_priority_and_stop()
{
    SoundScheduler sched;
    TEST_ASSERT_TRUE(sched.request(BoardFinderSound::RIDER_NOT_FOUND));
    TEST_ASSERT_FALSE(sched.request(BoardFinderSound::RIDER_NOT_FOUND)); // duplicate ignored
    TEST_ASSERT_TRUE(sched.request(BoardFinderSound::SEPARATION_DETECTED)); // higher wins
    TEST_ASSERT_EQUAL(static_cast<int>(BoardFinderSound::SEPARATION_DETECTED), static_cast<int>(sched.active()));

    SoundAction a{};
    TEST_ASSERT_TRUE(sched.tick(1000, a));
    TEST_ASSERT_EQUAL(static_cast<int>(SoundAction::Type::StartTone), static_cast<int>(a.type));
    TEST_ASSERT_EQUAL_UINT16(LOW_TONE_HZ, a.freq_hz);

    sched.stop();
    TEST_ASSERT_FALSE(sched.isPlaying());
    TEST_ASSERT_FALSE(sched.tick(2000, a));
}

void test_sound_scheduler_sequence_timing()
{
    SoundScheduler sched;
    TEST_ASSERT_TRUE(sched.request(BoardFinderSound::RIDER_DETECTED));
    SoundAction a{};
    TEST_ASSERT_TRUE(sched.tick(0, a));
    TEST_ASSERT_EQUAL_UINT16(HIGH_TONE_HZ, a.freq_hz);
    TEST_ASSERT_FALSE(sched.tick(50, a)); // still in first tone
    TEST_ASSERT_TRUE(sched.tick(120, a)); // gap
    TEST_ASSERT_EQUAL(static_cast<int>(SoundAction::Type::Silence), static_cast<int>(a.type));
    TEST_ASSERT_TRUE(sched.tick(220, a)); // second high
    TEST_ASSERT_EQUAL_UINT16(HIGH_TONE_HZ, a.freq_hz);
}

void setup()
{
    UNITY_BEGIN();
    RUN_TEST(test_encode_decode_get_config);
    RUN_TEST(test_encode_decode_set_config_on);
    RUN_TEST(test_encode_decode_config_response_off);
    RUN_TEST(test_malformed_empty);
    RUN_TEST(test_idempotent_set_encoding);
    RUN_TEST(test_v2_arm_and_status);
    RUN_TEST(test_parse_rider_service_data_ok);
    RUN_TEST(test_parse_wrong_uuid);
    RUN_TEST(test_parse_truncated_node_id);
    RUN_TEST(test_same_burst_grouping);
    RUN_TEST(test_sound_pattern_rider_detected);
    RUN_TEST(test_sound_pattern_synchronized);
    RUN_TEST(test_sound_pattern_separation);
    RUN_TEST(test_sound_scheduler_priority_and_stop);
    RUN_TEST(test_sound_scheduler_sequence_timing);
    UNITY_END();
}

void loop() {}
