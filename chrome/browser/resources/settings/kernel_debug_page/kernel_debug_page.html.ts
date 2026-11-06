// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {html} from '//resources/lit/v3_0/lit.rollup.js';

import type {SettingsKernelDebugPageElement} from './kernel_debug_page.js';

export function getHtml(this: SettingsKernelDebugPageElement) {
  return html`<settings-section page-title="Hello chromium"
      class="cr-centered-card-container">
    <div class="cr-row two-line first">
      <div class="flex cr-padded-text">
        <div>Chromium 版本</div>
        <div class="secondary">
          ${this.debugInfo_?.version ?? '读取中...'}
        </div>
      </div>
    </div>
    <div class="cr-row two-line">
      <div class="flex cr-padded-text">
        <div>浏览器进程 PID</div>
        <div class="secondary">
          ${this.debugInfo_?.processId ?? '读取中...'}
        </div>
      </div>
    </div>
    <div class="cr-row two-line">
      <div class="flex cr-padded-text">
        <div>浏览器进程运行时长</div>
        <div class="secondary">
          ${this.debugInfo_ ?
              this.formatUptime_(this.debugInfo_.uptimeSeconds) : '读取中...'}
        </div>
      </div>
    </div>
    <div class="cr-row two-line">
      <div class="flex cr-padded-text">
        <div>浏览器进程常驻内存（RSS）</div>
        <div class="secondary">${this.formatMemory_()}</div>
      </div>
    </div>
    <div class="cr-row two-line">
      <div class="flex cr-padded-text">
        <div>当前 Profile 的窗口 / 标签页</div>
        <div class="secondary">
          ${this.debugInfo_ ?
              `${this.debugInfo_.browserWindowCount} / ${this.debugInfo_.tabCount}` :
              '读取中...'}
        </div>
      </div>
    </div>
    <div class="cr-row two-line">
      <div class="flex cr-padded-text">
        <div>最近更新时间</div>
        <div class="secondary">${this.lastUpdated_ || '等待后端数据...'}</div>
      </div>
    </div>
    <div class="cr-row two-line">
      <div class="flex cr-padded-text">
        <div>当前 Profile 的标签页</div>
        <div class="secondary">
          ${this.debugInfo_ ? this.debugInfo_.tabs.length : '读取中...'}
        </div>
      </div>
    </div>
    <div class="tab-list">
      ${(this.debugInfo_?.tabs ?? []).map(tab => html`
        <div class="tab-item">
          <div class="tab-header">
            <button class="tab-expand-button"
                data-tab-id="${tab.id}"
                aria-expanded="${this.expandedTabId_ === tab.id}"
                @click="${this.onTabClick_}">
              <span class="tab-title">${tab.title || '（无标题）'}</span>
              <span class="tab-status">
                ${tab.isActive ? '活动标签页' : `窗口 ${tab.windowNumber}`}
              </span>
            </button>
            <cr-icon-button class="icon-delete-gray"
                data-tab-id="${tab.id}"
                title="关闭标签页"
                aria-label="关闭标签页：${tab.title || '无标题'}"
                @click="${this.onCloseTabClick_}">
            </cr-icon-button>
          </div>
          ${this.expandedTabId_ === tab.id ? html`
            ${this.detailsLoading_ ? html`
              <div class="cr-padded-text secondary">正在读取标签页信息...</div>
            ` : ''}
            ${!this.detailsLoading_ && this.tabDetails_ ? html`
              <div class="tab-details">
                <div>网址</div>
                <div class="tab-detail-value">${this.tabDetails_.url || '无'}</div>
                <div>渲染进程 PID</div>
                <div class="tab-detail-value">
                  ${this.tabDetails_.rendererProcessId ?? '暂不可用'}
                </div>
                <div>状态</div>
                <div class="tab-detail-value">
                  ${this.tabDetails_.isCrashed ? '渲染进程已崩溃' :
                      this.tabDetails_.isLoading ? '正在加载' : '运行中'}
                  ${this.tabDetails_.isVisible ? ' · 可见' : ' · 后台'}
                  ${this.tabDetails_.isAudible ? ' · 正在播放音频' : ''}
                </div>
                <div>最近活动</div>
                <div class="tab-detail-value">
                  ${this.formatLastActive_(
                      this.tabDetails_.secondsSinceLastActive)}
                </div>
              </div>
            ` : ''}
            ${!this.detailsLoading_ && !this.tabDetails_ ? html`
              <div class="cr-padded-text secondary">
                标签页已关闭或信息暂不可用
              </div>
            ` : ''}
          ` : ''}
        </div>
      `)}
    </div>
  </settings-section>`;
}
