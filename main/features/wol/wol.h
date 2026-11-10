#ifndef WOL_H
#define WOL_H

#include <string>
#include <vector>

// Wake-on-LAN for the user's computers. The magic packet goes from the device
// straight into the local network, so no server is involved and a computer does
// not have to be reachable by IP: only its MAC address is needed. The list of
// computers lives in NVS and is edited from the "Wake-on-LAN" tab of the page.
namespace wol {

// Used when a computer does not set its own values.
constexpr const char* kDefaultBroadcast = "255.255.255.255";
constexpr int kDefaultPort = 9;

struct Computer {
    // Stable identifier generated on add ("pc1", "pc2", ...), used by the page.
    std::string key;
    // What the user calls it, e.g. "Рабочий ПК"; also matched by voice.
    std::string name;
    // Upper-case "AA:BB:CC:DD:EE:FF".
    std::string mac;
    std::string broadcast = kDefaultBroadcast;
    int port = kDefaultPort;
    // Chosen in the page; "включи компьютер" without a name wakes this one.
    // At most one saved computer has it, the field is stored with the list.
    bool is_default = false;
};

// Reads the saved list. The single computer of the first version of this
// feature is returned as one entry named "Компьютер" until the list is saved.
std::vector<Computer> LoadComputers();

// First free "pc1", "pc2", ... identifier for a new computer.
std::string NextKey(const std::vector<Computer>& computers);

// Validates and saves the whole list to NVS: every entry needs a key, a name,
// a MAC address, an IPv4 broadcast address and a port in 1..65535.
bool SaveComputers(const std::vector<Computer>& computers, std::string& error);

// "aa-bb-cc-dd-ee-ff", "AA:BB:CC:DD:EE:FF" and "aabbccddeeff" all become
// "AA:BB:CC:DD:EE:FF"; anything else is rejected.
bool NormalizeMac(const std::string& text, std::string& mac);

// Sends six 0xFF bytes followed by sixteen copies of mac to broadcast:port.
bool SendMagicPacket(const std::string& mac, const std::string& broadcast, int port,
                     std::string& error);

// Finds a computer by key or by name, ignoring case. An empty selector means
// the computer marked as default, or "the only computer" when none is marked:
// it fails and lists the names when several are saved.
bool FindComputer(const std::vector<Computer>& computers, const std::string& selector,
                  Computer& computer, std::string& error);

// Resolves the selector against the saved list and sends the magic packet to
// `computer`, which the caller can use to name it in a reply.
bool Wake(const std::string& selector, Computer& computer, std::string& error);

}  // namespace wol

#endif  // WOL_H
