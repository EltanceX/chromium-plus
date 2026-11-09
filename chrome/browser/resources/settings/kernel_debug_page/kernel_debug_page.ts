// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import '../settings_page/settings_section.js';
import 'chrome://resources/cr_elements/cr_icon_button/cr_icon_button.js';
import 'chrome://resources/cr_elements/cr_button/cr_button.js';
import 'chrome://resources/cr_elements/cr_toggle/cr_toggle.js';
import '../relaunch_confirmation_dialog.js';

import {WebUiListenerMixinLit} from 'chrome://resources/cr_elements/web_ui_listener_mixin_lit.js';
import {css, CrLitElement} from '//resources/lit/v3_0/lit.rollup.js';
import {RouteObserverMixinLit} from '../router.js';
import type {Route} from '../router.js';
import {routes} from '../route.js';
import {RelaunchMixinLit, RestartType} from '../relaunch_mixin_lit.js';

import type {SearchResult} from '../search_settings.js';
import type {SettingsPlugin} from '../settings_main/settings_plugin.js';
import {getCss as getCrSharedStyle} from 'chrome://resources/cr_elements/cr_shared_style_lit.css.js';
import {getCss as getSettingsSharedStyle} from '../settings_shared_lit.css.js';

import type {
  KernelDebugInfo,
  KernelDebugTabDetails,
  NativePythonSettings,
} from './kernel_debug_page_browser_proxy.js';
import {KernelDebugPageBrowserProxyImpl} from './kernel_debug_page_browser_proxy.js';
import {getHtml} from './kernel_debug_page.html.js';

const SettingsKernelDebugPageElementBase =
    RelaunchMixinLit(RouteObserverMixinLit(WebUiListenerMixinLit(CrLitElement)));

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
        .python-origin {
          background: var(--cr-card-background-color);
          border: 1px solid var(--cr-separator-color);
          border-radius: 4px;
          box-sizing: border-box;
          color: var(--cr-primary-text-color);
          font: inherit;
          max-width: 100%;
          padding: 8px;
          width: 360px;
        }

        .python-actions {
          gap: 12px;
        }

        .python-message {
          overflow-wrap: anywhere;
        }

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
      pythonSettings_: {type: Object},
      pythonEnabled_: {type: Boolean},
      pythonUnrestricted_: {type: Boolean},
      pythonAllowedOrigin_: {type: String},
      pythonSaving_: {type: Boolean},
      pythonMessage_: {type: String},
    };
  }

  protected accessor debugInfo_: KernelDebugInfo|null = null;
  protected accessor lastUpdated_: string = '';
  protected accessor expandedTabId_: number|null = null;
  protected accessor tabDetails_: KernelDebugTabDetails|null = null;
  protected accessor detailsLoading_: boolean = false;
  protected accessor pythonSettings_: NativePythonSettings|null = null;
  protected accessor pythonEnabled_: boolean = false;
  protected accessor pythonUnrestricted_: boolean = false;
  protected accessor pythonAllowedOrigin_: string = '';
  protected accessor pythonSaving_: boolean = false;
  protected accessor pythonMessage_: string = '';

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
    this.loadPythonSettings_();
    this.updateMonitoring_();
  }

  override currentRouteChanged(newRoute: Route) {
    this.isActiveRoute_ = newRoute === routes.KERNEL_DEBUG;
    if (this.isActiveRoute_ && this.isReady_) {
      this.loadPythonSettings_();
    }
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

  private applyPythonSettings_(settings: NativePythonSettings) {
    this.pythonSettings_ = settings;
    this.pythonEnabled_ = settings.enabled;
    this.pythonUnrestricted_ = settings.unrestricted;
    this.pythonAllowedOrigin_ = settings.allowedOrigin;
  }

  private async loadPythonSettings_() {
    try {
      this.applyPythonSettings_(await this.browserProxy_.getPythonSettings());
    } catch {
      this.pythonMessage_ = '无法读取 Python 设置。';
    }
  }

  protected onPythonEnabledChange_(event: Event) {
    this.pythonEnabled_ = (event.target as HTMLInputElement).checked;
    this.pythonMessage_ = '';
  }

  protected onPythonUnrestrictedChange_(event: Event) {
    this.pythonUnrestricted_ = (event.target as HTMLInputElement).checked;
    this.pythonMessage_ = '';
  }

  protected onPythonOriginInput_(event: Event) {
    this.pythonAllowedOrigin_ = (event.target as HTMLInputElement).value;
    this.pythonMessage_ = '';
  }

  protected async onSavePythonSettingsClick_() {
    this.pythonSaving_ = true;
    try {
      this.applyPythonSettings_(await this.browserProxy_.setPythonSettings({
        enabled: this.pythonEnabled_, unrestricted: this.pythonUnrestricted_,
        allowedOrigin: this.pythonAllowedOrigin_.trim(),
      }));
      this.pythonMessage_ = '设置已保存。';
    } catch (error) {
      this.pythonMessage_ = String(error);
    } finally {
      this.pythonSaving_ = false;
    }
  }

  protected onPythonRelaunchClick_() {
    this.performRestart(RestartType.RELAUNCH);
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
