/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set sw=2 ts=8 et tw=80 : */

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_net_AltDataOutputStreamParent_h
#define mozilla_net_AltDataOutputStreamParent_h

#include "mozilla/net/PAltDataOutputStreamParent.h"
#include "nsIOutputStream.h"

namespace mozilla {
namespace net {

class AltDataOutputStreamParent
  : public PAltDataOutputStreamParent
  , public nsISupports
{
public:
  NS_DECL_ISUPPORTS
  explicit AltDataOutputStreamParent(nsIOutputStream* aStream);

  virtual bool RecvWriteData(const nsCString& data) override;
  virtual bool RecvClose() override;
  virtual void ActorDestroy(ActorDestroyReason aWhy) override;
  void SetError(nsresult status) { mStatus = status; }

private:
  virtual ~AltDataOutputStreamParent();
  nsCOMPtr<nsIOutputStream> mOutputStream;
  nsresult mStatus;
};

} // namespace net
} // namespace mozilla

#endif // mozilla_net_AltDataOutputStreamParent_h
