#include "_registry.h"

const BleSpamProtocol* ble_spam_protocols[] = {
    &ble_spam_protocol_continuity,
};

const size_t ble_spam_protocols_count = COUNT_OF(ble_spam_protocols);
