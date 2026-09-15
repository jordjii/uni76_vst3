#include "LicenseState.h"
#include "LicenseCrypto.h"
#include "LicensePublicKey.h"

namespace uni76
{
    namespace
    {
        constexpr const char* licenseFileName = "license.uni76lic";
        constexpr const char* machineIdFileName = "machine-id.txt";
        constexpr const char* licenseXmlTag = "UNI76License";
        constexpr const char* productToken = "UNI76";
        constexpr int licenseFormatVersion = 1;

        juce::String currentMachineId()
        {
            return juce::SystemStats::getUniqueDeviceID();
        }
    }

    juce::File LicenseState::getLicenseDirectory()
    {
        return juce::File::getSpecialLocation (juce::File::SpecialLocationType::userApplicationDataDirectory)
                   .getChildFile ("Nostalgia Audio")
                   .getChildFile ("UNI 76")
                   .getChildFile ("License");
    }

    juce::File LicenseState::getMachineIdFile() { return getLicenseDirectory().getChildFile (machineIdFileName); }
    juce::File LicenseState::getLicenseFile()   { return getLicenseDirectory().getChildFile (licenseFileName); }

    void LicenseState::refresh()
    {
        auto dir = getLicenseDirectory();
        if (! dir.isDirectory())
            dir.createDirectory();

        // Refreshed on every load (cheap - a few lines of text), not only
        // when missing, so this can never go stale relative to whatever
        // machine it's actually running on.
        getMachineIdFile().replaceWithText (
            "UNI 76 - machine ID\r\n"
            "\r\n"
            "To receive a license for this computer, send this ID to\r\n"
            "Nostalgia Audio along with your purchase confirmation:\r\n"
            "\r\n"
            + currentMachineId() + "\r\n"
            "\r\n"
            "Once you receive your license file back, drop it into this\r\n"
            "same folder as \"license.uni76lic\" and restart your DAW.\r\n");

        licensed.store (false, std::memory_order_relaxed);

        auto licenseFile = getLicenseFile();
        if (! licenseFile.existsAsFile())
            return;

        auto xml = juce::XmlDocument::parse (licenseFile);
        if (xml == nullptr || xml->getTagName() != licenseXmlTag)
            return;

        if (xml->getIntAttribute ("version", 0) != licenseFormatVersion)
            return;

        const auto payload = xml->getStringAttribute ("payload");
        const auto signature = xml->getStringAttribute ("signature");
        if (! uni76::license::verifySignature (payload, signature, licensePublicKeyString))
            return;

        // Payload shape: "UNI76|1|<email>|<machineId1>|<machineId2>|<issuedDateISO>"
        // - machineId2 may be an empty token (single-machine license).
        const auto tokens = juce::StringArray::fromTokens (payload, "|", "");
        if (tokens.size() != 6 || tokens[0] != productToken || tokens[1].getIntValue() != licenseFormatVersion)
            return;

        const auto machineId1 = tokens[3];
        const auto machineId2 = tokens[4];
        const auto thisMachine = currentMachineId();
        if (machineId1 != thisMachine && machineId2 != thisMachine)
            return;

        licensed.store (true, std::memory_order_relaxed);
    }
}
