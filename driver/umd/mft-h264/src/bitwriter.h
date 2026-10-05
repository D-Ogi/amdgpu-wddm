// SPDX-License-Identifier: MIT
// RBSP bit writer and NAL assembly for H.264 (ITU-T Rec. H.264 clauses 7.3, 7.4.1, 9.1).
// The writer accumulates RBSP bits; EmitNal appends a start code prefix, the NAL header and the
// escaped RBSP (emulation prevention, clause 7.4.1.1) to a byte vector.

#pragma once
#include <stdint.h>
#include <vector>

namespace bc250h264 {

class BitWriter {
public:
    void Clear()
    {
        m_bytes.clear();
        m_acc = 0;
        m_accBits = 0;
    }

    // Writes the low n bits of value, most significant first. n <= 32.
    void U(uint32_t n, uint32_t value)
    {
        while (n > 0) {
            uint32_t take = n < (8u - m_accBits) ? n : (8u - m_accBits);
            uint32_t shift = n - take;
            uint32_t bits = (value >> shift) & ((1u << take) - 1u);
            m_acc = static_cast<uint8_t>((m_acc << take) | bits);
            m_accBits += take;
            n -= take;
            if (m_accBits == 8) {
                m_bytes.push_back(m_acc);
                m_acc = 0;
                m_accBits = 0;
            }
        }
    }

    void Flag(bool v) { U(1, v ? 1u : 0u); }

    // ue(v), clause 9.1. codeNum must be < 2^31 - 1.
    void UE(uint32_t codeNum)
    {
        uint32_t v = codeNum + 1;
        uint32_t len = 0;
        while ((v >> len) > 1u) {
            ++len;
        }
        U(len, 0);
        U(len + 1, v);
    }

    // se(v), clause 9.1.1.
    void SE(int32_t value)
    {
        uint32_t codeNum = value > 0 ? static_cast<uint32_t>(2 * value - 1)
                                     : static_cast<uint32_t>(-2 * value);
        UE(codeNum);
    }

    // me(v) is written by the caller as UE(CbpToCodeNum(...)).

    // rbsp_trailing_bits, clause 7.3.2.11.
    void RbspTrailingBits()
    {
        U(1, 1);
        while (m_accBits != 0) {
            U(1, 0);
        }
    }

    size_t BitCount() const { return m_bytes.size() * 8 + m_accBits; }
    const std::vector<uint8_t>& Rbsp() const { return m_bytes; }

private:
    std::vector<uint8_t> m_bytes;
    uint8_t m_acc = 0;
    uint32_t m_accBits = 0;
};

// Appends start code (4 bytes), nal header and escaped RBSP. The RBSP must already be byte aligned.
inline void EmitNal(std::vector<uint8_t>& out, uint32_t nalRefIdc, uint32_t nalUnitType,
                    const std::vector<uint8_t>& rbsp)
{
    out.push_back(0);
    out.push_back(0);
    out.push_back(0);
    out.push_back(1);
    out.push_back(static_cast<uint8_t>(((nalRefIdc & 3u) << 5) | (nalUnitType & 31u)));
    uint32_t zeros = 0;
    for (uint8_t b : rbsp) {
        if (zeros >= 2 && b <= 3) {
            out.push_back(0x03);
            zeros = 0;
        }
        out.push_back(b);
        zeros = (b == 0) ? zeros + 1 : 0;
    }
}

} // namespace bc250h264
