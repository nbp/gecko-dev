/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_SRICheck_h
#define mozilla_dom_SRICheck_h

#include "mozilla/CORSMode.h"
#include "nsCOMPtr.h"
#include "nsICryptoHash.h"
#include "SRIMetadata.h"

class nsIChannel;
class nsIDocument;
class nsIUnicharStreamLoader;

namespace mozilla {
namespace dom {

class SRICheck final
{
public:
  static const uint32_t MAX_METADATA_LENGTH = 24*1024;
  static const uint32_t MAX_METADATA_TOKENS = 512;

  /**
   * Parse the multiple hashes specified in the integrity attribute and
   * return the strongest supported hash.
   */
  static nsresult IntegrityMetadata(const nsAString& aMetadataList,
                                    const nsIDocument* aDocument,
                                    SRIMetadata* outMetadata);

  /**
   * Process the integrity attribute of the element.  A result of false
   * must prevent the resource from loading.
   */
  static nsresult VerifyIntegrity(const SRIMetadata& aMetadata,
                                  nsIUnicharStreamLoader* aLoader,
                                  const CORSMode aCORSMode,
                                  const nsAString& aString,
                                  const nsIDocument* aDocument);
};

class SRICheckDataVerifier final
{
  public:
    SRICheckDataVerifier(const SRIMetadata& aMetadata,
                         const nsIDocument* aDocument);

    // Append the following bytes to the content used to compute the hash. Once
    // all bytes are streamed, use the Verify function to check the integrity.
    nsresult Update(uint32_t aStringLen, const uint8_t* aString);

    // Verify that the computed hash corresponds to the metadata.
    nsresult Verify(const SRIMetadata& aMetadata, nsIChannel* aChannel,
                    const CORSMode aCORSMode, const nsIDocument* aDocument);

    bool IsComplete() const {
      return mComplete;
    }

    // Return the length of the serialized hash.
    uint32_t SerializedHashLength();
    static uint32_t UnknownSerializedHashLength();
    static nsresult HashLength(uint32_t aDataLen, const uint8_t* aData, uint32_t* length);

    // Read the computed hash from a cache. The array should be at least the
    // same size or larger than the value returned by SerializedHashLength.
    // Once the deserialized function is called, one can call the Verify
    // function to check the integrity with the metadata present in the
    // document, in case the document metadata changed.
    nsresult DeserializeVerifiedHash(uint32_t aDataLen, const uint8_t* aData);

    // Write the computed hash in a buffer which is at least the size returned
    // by SerializedHashLength.
    nsresult SerializeVerifiedHash(uint32_t aDataLen, uint8_t* aData);
    static nsresult SerializeUnknownHash(uint32_t aDataLen, uint8_t* aData);

  private:
    nsCOMPtr<nsICryptoHash> mCryptoHash;
    nsAutoCString           mComputedHash;
    size_t                  mBytesHashed;
    uint32_t                mHashLength;
    int8_t                  mHashType;
    bool                    mInvalidMetadata;
    bool                    mComplete;

    nsresult EnsureCryptoHash();
    nsresult Finish();
    nsresult VerifyHash(const SRIMetadata& aMetadata, uint32_t aHashIndex,
                        const nsIDocument* aDocument);
};

} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_SRICheck_h
