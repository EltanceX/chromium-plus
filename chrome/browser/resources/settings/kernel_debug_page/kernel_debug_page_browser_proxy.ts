// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {sendWithPromise} from 'chrome://resources/js/cr.js';

export interface KernelDebugTab {
  id: number;
  title: string;
  isActive: boolean;
  windowNumber: number;
}

export interface KernelDebugTabDetails extends KernelDebugTab {
  url: string;
  isVisible: boolean;
  isLoading: boolean;
  isCrashed: boolean;
  isAudible: boolean;
  rendererProcessId: number|null;
  secondsSinceLastActive: number|null;
}

export interface KernelDebugInfo {
  version: string;
  processId: number;
  uptimeSeconds: number;
  residentSetMb: number|null;
  browserWindowCount: number;
  tabCount: number;
  tabs: KernelDebugTab[];
}

export interface KernelDebugPageBrowserProxy {
  startMonitoring(): void;
  stopMonitoring(): void;
  getTabDetails(tabId: number): Promise<KernelDebugTabDetails|null>;
  closeTab(tabId: number): void;
}

export class KernelDebugPageBrowserProxyImpl implements
    KernelDebugPageBrowserProxy {
  startMonitoring() {
    chrome.send('startKernelDebugMonitoring');
  }

  stopMonitoring() {
    chrome.send('stopKernelDebugMonitoring');
  }

  getTabDetails(tabId: number): Promise<KernelDebugTabDetails|null> {
    return sendWithPromise('getKernelDebugTabDetails', tabId);
  }

  closeTab(tabId: number) {
    chrome.send('closeKernelDebugTab', [tabId]);
  }

  static getInstance(): KernelDebugPageBrowserProxy {
    return instance || (instance = new KernelDebugPageBrowserProxyImpl());
  }

  static setInstance(obj: KernelDebugPageBrowserProxy) {
    instance = obj;
  }
}

let instance: KernelDebugPageBrowserProxy|null = null;
