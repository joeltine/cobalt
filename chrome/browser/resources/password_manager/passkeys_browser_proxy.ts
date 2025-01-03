// Copyright 2023 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {sendWithPromise} from 'chrome://resources/js/cr.js';

export interface PasskeysBrowserProxy {
  /**
   * Determines whether any passkeys exist on the local device.
   * May report false positives if the last passkey was recently deleted.
   */
  hasPasskeys(): Promise<boolean>;
}

export class PasskeysBrowserProxyImpl implements PasskeysBrowserProxy {
  hasPasskeys() {
    return sendWithPromise('passkeysHasPasskeys');
  }

  static getInstance(): PasskeysBrowserProxy {
    return passkeysProxyInstance ||
        (passkeysProxyInstance = new PasskeysBrowserProxyImpl());
  }

  static setInstance(obj: PasskeysBrowserProxy) {
    passkeysProxyInstance = obj;
  }
}

let passkeysProxyInstance: PasskeysBrowserProxy|null = null;
