#pragma once

namespace uni76
{
    /*
        UNI 76 - embedded RSA public key for offline license verification.

        Generated once via `Tools/LicenseKeygen genkeys` (see
        docs/LICENSING.md) and pasted here. Safe to commit - it's a public
        key. The matching PRIVATE key must never be committed or shared;
        it lives only under Tools/LicenseKeygen/keys/ on the developer's
        own machine (gitignored - see .gitignore) and is what actually
        issues licenses via `Tools/LicenseKeygen issue`.

        Generated 2026-09-15 (3072-bit) - see docs/LICENSING.md.
    */
    inline constexpr const char* licensePublicKeyString =
        "5,805bcb3d18c1e7795871d2eca24517e76c835fe413532fd3917c233f9518d870bd7c9d2c8268a8f076e49ac6c4a50bf33efd8ae57c60f71a9e7c0725d7f472adf6418c6dfed84afe65082613d2a13b90c63886b8d4bec1b03d649a21c14dc42cae1514c0d85dc372a8a466b5113e7491a7ed56cef1e728a4589711a3c2636540130b8870136bc5d5a3d9ebbc5581ec0ea1c2cfabed5fd408846d94c7ef8969e7d72d0fee9a747bda69ea6ac35c1a171371fad65d977bf0f1f99994a6da8aaddb8f90053aaefb4cf7e2a0479567aab17ec10dcabd3e3ce28cb2ced86eba206a23d26ad54e56334156b98fe6c05788334ad4ceeb55e74bd83cc6f3fa99b4b675e0e783969e36ebec395f98068bda51a988b15ecb9e2e7553344be3f27e7b76ed48cb511c5b615fb108f73043b42c7e38b9b68a949f95ce71335bfd690112eb156de97d098132b807195e18dc58503fd8a95a25efa76a8c7078e1fde0508c348774f4f76f9d60aa106a02a8f4445fc8667001123dcf4b09bf36f1940c636933a513";
}
