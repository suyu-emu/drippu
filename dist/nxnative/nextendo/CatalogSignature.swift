// SPDX-License-Identifier: GPL-3.0-or-later
import CryptoKit
import Foundation

/// Verify the decoded catalog bytes with the public key published by Nextendo.
@_cdecl("drippu_verify_catalog")
public func verifyCatalog(_ payload: UnsafePointer<UInt8>, _ length: Int,
                          _ signature: UnsafePointer<UInt8>, _ signatureLength: Int) -> Int32 {
    guard signatureLength == 64, length > 0, length <= 1_048_576,
          let bytes = Data(base64Encoded: "xI+atR6VesrCH89ZDZD4t3WQLcYOFEhk5F0HLMzgWk4="),
          let key = try? Curve25519.Signing.PublicKey(rawRepresentation: bytes) else { return 0 }
    return key.isValidSignature(Data(bytes: signature, count: signatureLength),
                                for: Data(bytes: payload, count: length)) ? 1 : 0
}
