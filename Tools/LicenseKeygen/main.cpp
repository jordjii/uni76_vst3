/*
    UNI 76 - offline license keygen (developer-only tool).

    Never shipped to customers, never part of the plugin build (see
    UNI76_BUILD_TOOLS in the root CMakeLists.txt - default OFF). See
    docs/LICENSING.md for the full manual issue workflow this exists to
    support.

    Two subcommands:

      UNI76LicenseKeygen genkeys [--bits N] [--out-dir DIR]
          Generates a fresh RSA keypair (default 3072-bit) and writes
          public.key.txt / private.key.txt under DIR (default "keys" in
          the current directory). Run ONCE ever, not per sale. The public
          key then gets pasted into Source/Core/LicensePublicKey.h and
          committed; the private key must never be committed or shared -
          see Tools/LicenseKeygen/keys/.gitignore.

      UNI76LicenseKeygen issue --email E --machine1 ID [--machine2 ID]
                                [--private-key-file FILE] --out FILE
          Builds and signs a license for one customer (up to 2 machines),
          writing a .uni76lic XML file to send back to them. Run once per
          sale/support request.
*/

#include <iostream>

#include <juce_core/juce_core.h>
#include "../../Source/Core/LicenseCrypto.h"

namespace
{
    constexpr const char* licenseXmlTag = "UNI76License";
    constexpr const char* productToken = "UNI76";
    constexpr int licenseFormatVersion = 1;

    juce::String getOption (const juce::StringArray& args, const juce::String& name, const juce::String& fallback = {})
    {
        const auto index = args.indexOf (name);
        if (index >= 0 && index + 1 < args.size())
            return args[index + 1];
        return fallback;
    }

    void printUsage()
    {
        std::cout
            << "UNI 76 offline license keygen\n\n"
            << "Usage:\n"
            << "  UNI76LicenseKeygen genkeys [--bits N] [--out-dir DIR]\n"
            << "  UNI76LicenseKeygen issue --email E --machine1 ID [--machine2 ID]\n"
            << "                            [--private-key-file FILE] --out FILE\n\n"
            << "See docs/LICENSING.md for the full workflow.\n";
    }

    int runGenKeys (const juce::StringArray& args)
    {
        const auto bits = getOption (args, "--bits", "3072").getIntValue();
        const auto outDir = juce::File::getCurrentWorkingDirectory()
                                 .getChildFile (getOption (args, "--out-dir", "keys"));

        if (! outDir.isDirectory() && ! outDir.createDirectory())
        {
            std::cerr << "Could not create output directory: " << outDir.getFullPathName() << "\n";
            return 1;
        }

        std::cout << "Generating a " << bits << "-bit RSA keypair - this can take a little while...\n";

        juce::RSAKey publicKey, privateKey;
        juce::RSAKey::createKeyPair (publicKey, privateKey, bits);

        auto publicKeyFile = outDir.getChildFile ("public.key.txt");
        auto privateKeyFile = outDir.getChildFile ("private.key.txt");

        if (! publicKeyFile.replaceWithText (publicKey.toString())
            || ! privateKeyFile.replaceWithText (privateKey.toString()))
        {
            std::cerr << "Failed to write key files under " << outDir.getFullPathName() << "\n";
            return 1;
        }

        std::cout << "\nWrote:\n"
                   << "  " << publicKeyFile.getFullPathName() << "  (safe to commit)\n"
                   << "  " << privateKeyFile.getFullPathName() << "  (NEVER commit or share this)\n\n"
                   << "Next step: paste the public key below into\n"
                   << "Source/Core/LicensePublicKey.h's licensePublicKeyString, then rebuild the plugin.\n\n"
                   << "---- public key ----\n"
                   << publicKey.toString() << "\n"
                   << "---------------------\n";
        return 0;
    }

    int runIssue (const juce::StringArray& args)
    {
        const auto email = getOption (args, "--email");
        const auto machine1 = getOption (args, "--machine1");
        const auto machine2 = getOption (args, "--machine2");
        const auto outPath = getOption (args, "--out");
        const auto privateKeyFile = juce::File::getCurrentWorkingDirectory()
                                         .getChildFile (getOption (args, "--private-key-file", "keys/private.key.txt"));

        if (email.isEmpty() || machine1.isEmpty() || outPath.isEmpty())
        {
            std::cerr << "Missing required option(s). --email, --machine1 and --out are required.\n\n";
            printUsage();
            return 1;
        }

        if (! privateKeyFile.existsAsFile())
        {
            std::cerr << "Private key file not found: " << privateKeyFile.getFullPathName()
                       << "\nRun 'genkeys' first (once ever), or pass --private-key-file.\n";
            return 1;
        }

        const auto privateKeyString = privateKeyFile.loadFileAsString().trim();
        const auto issuedDate = juce::Time::getCurrentTime().formatted ("%Y-%m-%d");

        // "|"-delimited, matching Source/Core/LicenseState.cpp's own parse -
        // see LicenseCrypto.h's doc comment for why this stays this simple
        // (a closed, single-signer system, not a general protocol).
        const juce::String payload = juce::String (productToken) + "|"
                                    + juce::String (licenseFormatVersion) + "|"
                                    + email + "|"
                                    + machine1 + "|"
                                    + machine2 + "|"
                                    + issuedDate;

        const auto signature = uni76::license::signPayload (payload, privateKeyString);

        juce::XmlElement root (licenseXmlTag);
        root.setAttribute ("version", licenseFormatVersion);
        root.setAttribute ("payload", payload);
        root.setAttribute ("signature", signature);

        juce::File outFile (outPath);
        if (! outFile.getParentDirectory().createDirectory() || ! root.writeTo (outFile))
        {
            std::cerr << "Failed to write license file: " << outFile.getFullPathName() << "\n";
            return 1;
        }

        std::cout << "Wrote license file: " << outFile.getFullPathName() << "\n"
                   << "  email:    " << email << "\n"
                   << "  machine1: " << machine1 << "\n"
                   << "  machine2: " << (machine2.isEmpty() ? juce::String ("(none)") : machine2) << "\n"
                   << "  issued:   " << issuedDate << "\n\n"
                   << "Send this file to the customer - they should drop it into their\n"
                   << "\"Nostalgia Audio/UNI 76/License\" folder as \"license.uni76lic\".\n";
        return 0;
    }
}

int main (int argc, char* argv[])
{
    juce::StringArray args;
    for (int i = 1; i < argc; ++i)
        args.add (juce::String (argv[i]));

    if (args.isEmpty())
    {
        printUsage();
        return 1;
    }

    if (args[0] == "genkeys")
        return runGenKeys (args);

    if (args[0] == "issue")
        return runIssue (args);

    printUsage();
    return 1;
}
