// Exercise the real QMX PAL adapter and CAT parser with an in-memory transport.
// No serial ports, USB devices, audio devices, or GUI are opened.
#include "radio.h"
#include "pal.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

namespace {
bool connected = false;
std::string incoming;
std::string outgoing;

void expectSent(const char *expected)
{
    if (outgoing != expected) {
        std::fprintf(stderr, "Expected CAT: %s\nActual CAT: %s\n",
                     expected, outgoing.c_str());
        assert(false);
    }
    outgoing.clear();
}

void receive(const char *reply)
{
    incoming += reply;
    while (!incoming.empty()) pal::radio::poll();
}

void expectBand(int slot, int radio_index, const char *name)
{
    const auto *band = pal::radio::getBand(slot);
    assert(band);
    assert(band->index == radio_index);
    assert(std::strcmp(band->name, name) == 0);
}

void expectEnumeration(int first)
{
    std::string expected;
    for (int i = first; i < first + 4; ++i)
        expected += "MMBand config.|Band name (m)[" + std::to_string(i) + "];";
    expectSent(expected.c_str());
}
} // namespace

namespace pal {
bool catIsConnected() { return connected; }
int catSend(const char *data, int len)
{
    if (!connected) return -1;
    outgoing.append(data, static_cast<size_t>(len));
    return len;
}
int catRecv(char *buf, int max_len)
{
    // Fragment replies, including escape sequences, across poll calls.
    const int size = std::min({max_len, 7, static_cast<int>(incoming.size())});
    std::memcpy(buf, incoming.data(), static_cast<size_t>(size));
    incoming.erase(0, static_cast<size_t>(size));
    return size;
}
} // namespace pal

int main()
{
    using namespace pal::radio;
    assert(init());
    assert(getState() == State::Offline);
    assert(getInputOffsetHz() == 12000);
    assert(!bandSelectionReadOnly());
    assert(!txTestSupported());
    assert(getTxTestLimitMs() == 0);
    assert(!requestTransmit(true));
    assert(!requestTransmit(false));
    assert(!powerCycleAcknowledgementRequired());
    assert(!acknowledgePowerCycle());
    assert(getBandCount() == 0);
    assert(getBand(-1) == nullptr);
    assert(getBand(0) == nullptr);
    tick1Hz();
    expectSent("");

    connected = true;
    assert(getState() == State::Unmanaged); // CAT adapter does not query PTT.
    tick1Hz();
    expectSent("Q91;MMCW|Choose filters|0=ENABLED;MMCW|Choose filters|4=ENABLED;");
    tick1Hz();
    expectSent("MMCW|CW passband=250;MMCW|CW center=625;MMCW|CW offset=625;");
    tick1Hz();
    expectSent("MMCW|Choose filters|1=DISABLED;MMCW|Choose filters|2=DISABLED;"
               "MMCW|Choose filters|3=DISABLED;MMCW|Choose filters|5=DISABLED;"
               "MMCW|Choose filters|6=DISABLED;MMCW|Choose filters|7=DISABLED;");

    tick1Hz();
    expectEnumeration(0);
    receive("\x1b[1;254H\x1b[0;0HMM160;MM0;MM80;MM40;");
    assert(!isBandEnumDone());
    assert(getBandCount() == 3);
    expectBand(0, 0, "160");
    expectBand(1, 2, "80");
    expectBand(2, 3, "40");

    tick1Hz();
    expectEnumeration(4);
    receive("MM30;MM20;MM0;MM17;");
    tick1Hz();
    expectEnumeration(8);
    receive("MM15;MM12;MM10;MM6;");
    tick1Hz();
    expectEnumeration(12);
    receive("MM0;MM0;MM0;MM0;");
    tick1Hz();
    expectSent("");
    assert(isBandEnumDone());
    assert(getBandCount() == 10);
    expectBand(4, 5, "20");
    expectBand(5, 7, "17");
    assert(getBand(10) == nullptr);

    const auto *stable_band = getBand(4);
    receive("FA00014074000;MD3;BN05;");
    assert(stable_band == getBand(4));
    assert(std::strcmp(stable_band->name, "20") == 0);
    assert(getVfoFreq() == 14074000);
    assert(getMode() == 3);
    assert(getCwOffset() == 625);
    assert(std::strcmp(getModeStr(), "CW") == 0);
    assert(getBandIndex() == 5);
    assert(getState() == State::Unmanaged);

    setVfoFreq(7075000);
    expectSent("FA00007075000;");
    assert(getVfoFreq() == 7075000);
    suppressPolling(true);
    tick1Hz();
    expectSent("");
    suppressPolling(false);
    tick1Hz(); // Existing post-tune polling pause is preserved.
    expectSent("");
    tick1Hz();
    expectSent("FA;MD;BN;");

    setBandIndex(5);
    expectSent("BN5;FA;MD;");
    setBandIndex(-1);
    setBandIndex(MAX_BANDS);
    expectSent("");
    setCwFilter("50");
    expectSent("MMCW|CW passband=50;");
    receive("MD6;");
    assert(getCwOffset() == 0);
    assert(std::strcmp(getModeStr(), "DIGI") == 0);
    assert(!requestTransmit(true));
    expectSent("");

    connected = false;
    assert(getState() == State::Offline);
    tick1Hz();
    expectSent("");
    shutdown();
    expectSent("");
    std::puts("QMX radio PAL regression PASS");
}
