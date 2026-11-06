// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import '../settings_page/settings_section.js';
import 'chrome://resources/cr_elements/cr_icon_button/cr_icon_button.js';

import {WebUiListenerMixinLit} from 'chrome://resources/cr_elements/web_ui_listener_mixin_lit.js';
import {css, CrLitElement} from '//resources/lit/v3_0/lit.rollup.js';
import {RouteObserverMixinLit} from '../router.js';
import type {Route} from '../router.js';
import {routes} from '../route.js';

import type {SearchResult} from '../search_settings.js';
import type {SettingsPlugin} from '../settings_main/settings_plugin.js';
import {getCss as getCrSharedStyle} from 'chrome://resources/cr_elements/cr_shared_style_lit.css.js';
import {getCss as getSettingsSharedStyle} from '../settings_shared_lit.css.js';

import type {
  KernelDebugInfo,
  KernelDebugTabDetails,
} from './kernel_debug_page_browser_proxy.js';
import {KernelDebugPageBrowserProxyImpl} from './kernel_debug_page_browser_proxy.js';
import {getHtml} from './kernel_debug_page.html.js';

const SettingsKernelDebugPageElementBase =
    RouteObserverMixinLit(WebUiListenerMixinLit(CrLitElement));

export class SettingsKernelDebugPageElement extends
    SettingsKernelDebugPageElementBase implements SettingsPlugin {
  static get is() {
    return 'settings-kernel-debug-page';
  }

  static override get styles() {
    return [
      getCrSharedStyle(),
      getSettingsSharedStyle(),
      css`
        .tab-list {
          padding: 0 16px;
        }

        .tab-item {
          border-top: 1px solid var(--cr-separator-color);
        }

        .tab-header {
          align-items: center;
          display: flex;
          min-height: 48px;
        }

        .tab-expand-button {
          align-items: center;
          background: transparent;
          border: 0;
          color: inherit;
          cursor: pointer;
          display: flex;
          flex: 1;
          font: inherit;
          gap: 12px;
          min-width: 0;
          padding: 12px 8px;
          text-align: start;
        }

        .tab-title {
          flex: 1;
          min-width: 0;
          overflow: hidden;
          text-overflow: ellipsis;
          white-space: nowrap;
        }

        .tab-status {
          color: var(--cr-secondary-text-color);
          flex: none;
        }

        .tab-details {
          display: grid;
          gap: 8px 16px;
          grid-template-columns: minmax(90px, auto) minmax(0, 1fr);
          overflow-wrap: anywhere;
          padding: 0 16px 16px 24px;
        }

        .tab-detail-value {
          color: var(--cr-secondary-text-color);
          min-width: 0;
          white-space: pre-wrap;
        }
      `,
    ];
  }

  override render() {
    return getHtml.bind(this)();
  }

  static override get properties() {
    return {
      debugInfo_: {type: Object},
      lastUpdated_: {type: String},
      expandedTabId_: {type: Number},
      tabDetails_: {type: Object},
      detailsLoading_: {type: Boolean},
    };
  }

  protected accessor debugInfo_: KernelDebugInfo|null = null;
  protected accessor lastUpdated_: string = '';
  protected accessor expandedTabId_: number|null = null;
  protected accessor tabDetails_: KernelDebugTabDetails|null = null;
  protected accessor detailsLoading_: boolean = false;

  private browserProxy_ = KernelDebugPageBrowserProxyImpl.getInstance();
  private isActiveRoute_: boolean = false;
  private isMonitoring_: boolean = false;
  private isReady_: boolean = false;
  private detailsRequestVersion_: number = 0;

  override disconnectedCallback() {
    if (this.isMonitoring_) {
      this.browserProxy_.stopMonitoring();
      this.isMonitoring_ = false;
    }
    super.disconnectedCallback();
  }

  override firstUpdated() {
    this.addWebUiListener(
        'kernel-debug-info-changed', (info: KernelDebugInfo) => {
          this.debugInfo_ = info;
          this.lastUpdated_ = new Date().toLocaleTimeString();
          if (this.expandedTabId_ !== null) {
            if (!info.tabs.some(tab => tab.id === this.expandedTabId_)) {
              this.expandedTabId_ = null;
              this.tabDetails_ = null;
              this.detailsLoading_ = false;
              ++this.detailsRequestVersion_;
            } else {
              this.refreshTabDetails_(this.expandedTabId_);
            }
          }
        });
    this.isReady_ = true;
    this.updateMonitoring_();
  }

  override currentRouteChanged(newRoute: Route) {
    this.isActiveRoute_ = newRoute === routes.KERNEL_DEBUG;
    this.updateMonitoring_();
  }

  protected formatUptime_(seconds: number): string {
    const hours = Math.floor(seconds / 3600);
    const minutes = Math.floor((seconds % 3600) / 60);
    const remainingSeconds = seconds % 60;
    const days = Math.floor(hours / 24);
    const remainingHours = hours % 24;
    return days > 0 ?
        `${days} 天 ${remainingHours} 小时 ${minutes} 分` :
                       `${hours} 小时 ${minutes} 分 ${remainingSeconds} 秒`;
  }

  protected formatMemory_(): string {
    if (!this.debugInfo_) {
      return '读取中...';
    }
    return this.debugInfo_.residentSetMb === null ?
        '暂不可用' : `${this.debugInfo_.residentSetMb} MB`;
  }

  protected onTabClick_(event: Event) {
    const tabId = Number((event.currentTarget as HTMLElement).dataset['tabId']);
    if (!Number.isInteger(tabId)) {
      return;
    }

    if (this.expandedTabId_ === tabId) {
      this.expandedTabId_ = null;
      this.tabDetails_ = null;
      this.detailsLoading_ = false;
      ++this.detailsRequestVersion_;
      return;
    }

    this.expandedTabId_ = tabId;
    this.tabDetails_ = null;
    this.detailsLoading_ = true;
    this.refreshTabDetails_(tabId);
  }

  protected onCloseTabClick_(event: Event) {
    const tabId = Number((event.currentTarget as HTMLElement).dataset['tabId']);
    if (!Number.isInteger(tabId)) {
      return;
    }

    this.browserProxy_.closeTab(tabId);
    if (this.expandedTabId_ === tabId) {
      this.expandedTabId_ = null;
      this.tabDetails_ = null;
      this.detailsLoading_ = false;
      ++this.detailsRequestVersion_;
    }
  }

  protected formatLastActive_(seconds: number|null): string {
    if (seconds === null) {
      return '未知';
    }
    if (seconds < 60) {
      return `${seconds} 秒前`;
    }
    const minutes = Math.floor(seconds / 60);
    if (minutes < 60) {
      return `${minutes} 分钟前`;
    }
    return `${Math.floor(minutes / 60)} 小时前`;
  }

  private refreshTabDetails_(tabId: number) {
    const requestVersion = ++this.detailsRequestVersion_;
    this.browserProxy_.getTabDetails(tabId).then(details => {
      if (this.expandedTabId_ !== tabId ||
          this.detailsRequestVersion_ !== requestVersion) {
        return;
      }
      this.tabDetails_ = details;
      this.detailsLoading_ = false;
    }).catch(() => {
      if (this.expandedTabId_ === tabId &&
          this.detailsRequestVersion_ === requestVersion) {
        this.tabDetails_ = null;
        this.detailsLoading_ = false;
      }
    });
  }

  private updateMonitoring_() {
    if (!this.isReady_) {
      return;
    }

    if (this.isActiveRoute_ && !this.isMonitoring_) {
      this.browserProxy_.startMonitoring();
      this.isMonitoring_ = true;
    } else if (!this.isActiveRoute_ && this.isMonitoring_) {
      this.browserProxy_.stopMonitoring();
      this.isMonitoring_ = false;
    }
  }

  searchContents(query: string): Promise<SearchResult> {
    return Promise.resolve({
      canceled: false,
      matchCount: 0,
      wasClearSearch: query === '',
    });
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'settings-kernel-debug-page': SettingsKernelDebugPageElement;
  }
}

customElements.define(
    SettingsKernelDebugPageElement.is, SettingsKernelDebugPageElement);
