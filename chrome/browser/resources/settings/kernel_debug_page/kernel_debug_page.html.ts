// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {html} from '//resources/lit/v3_0/lit.rollup.js';

import {RestartType} from '../relaunch_mixin_lit.js';

import type {SettingsKernelDebugPageElement} from './kernel_debug_page.js';

export function getHtml(this: SettingsKernelDebugPageElement) {
  return html`<settings-section page-title="Python 解释器"
      class="cr-centered-card-container">
    <div class="cr-row two-line first">
      <div class="flex cr-padded-text">
        <div>启用 Python 解释器</div>
        <div class="secondary">执行 HTML 中原生 &lt;pyscript&gt; 内联代码。保存后重启浏览器生效。</div>
      </div>
      <cr-toggle id="pythonEnabled" aria-label="启用 Python 解释器"
          .checked="${this.pythonEnabled_}"
          ?disabled="${!this.pythonSettings_?.supported || this.pythonSaving_}"
          @change="${this.onPythonEnabledChange_}"></cr-toggle>
    </div>
    <div class="cr-row two-line">
      <div class="flex cr-padded-text">
        <div>放开 Python 的站点与页面执行限制</div>
        <div class="secondary">
          允许普通网页、本地 HTML 及子框架执行 Python，忽略来源白名单、页面脚本开关、CSP 和 sandbox 的脚本限制。
          仅对可信内容开启。
        </div>
      </div>
      <cr-toggle id="pythonUnrestricted" aria-label="放开 Python 的站点与页面执行限制"
          .checked="${this.pythonUnrestricted_}"
          ?disabled="${!this.pythonSettings_?.supported || !this.pythonEnabled_ || this.pythonSaving_}"
          @change="${this.onPythonUnrestrictedChange_}"></cr-toggle>
    </div>
    <div class="cr-row two-line">
      <div class="flex cr-padded-text">
        <div id="pythonOriginLabel">受限模式允许的来源</div>
        <div class="secondary">精确的本地 HTTP(S) 来源，包含端口；例如 http://127.0.0.1:8000。</div>
        <input id="pythonAllowedOrigin" class="python-origin" type="text"
            aria-labelledby="pythonOriginLabel" spellcheck="false" maxlength="2048"
            .value="${this.pythonAllowedOrigin_}"
            ?disabled="${!this.pythonSettings_?.supported || this.pythonUnrestricted_ || this.pythonSaving_}"
            @input="${this.onPythonOriginInput_}">
      </div>
    </div>
    <div class="cr-row two-line">
      <div class="flex cr-padded-text">
        <div>当前生效状态</div>
        <div id="pythonActiveState" class="secondary">
          ${!this.pythonSettings_ ? '读取中...' :
              !this.pythonSettings_.supported ? '当前平台不支持（仅 Windows x64）' :
              !this.pythonSettings_.activeEnabled ? '已关闭' :
              this.pythonSettings_.activeUnrestricted ? '已启用 · 放开页面限制' :
              `已启用 · 受限来源 ${this.pythonSettings_.activeAllowedOrigin || '未配置'}`}
        </div>
        <div class="secondary">
          Python 始终在 Renderer 沙箱内运行，系统权限与渲染进程相同。
          WebUI、扩展及 MCBE / MCJE 页面不执行 Python；内联解析规则、DOM 接口及执行预算保持生效。
        </div>
        <div class="secondary">初始化预算：单块约 100 ms，累计约 250 ms；输出 16 KiB，DOM 调用 1024 次。</div>
        <div class="secondary">点击预算：每回调约 50 ms、输出 4 KiB、DOM 调用 128 次。动画帧预算：每帧约 8 ms，最近 1 秒帧执行累计约 200 ms；每帧同样限制输出 4 KiB、DOM 调用 128 次。</div>
        <div class="secondary">交互总预算：点击与动画帧共享每文档最近 1 秒累计约 250 ms、输出 16 KiB、DOM 调用 1024 次。窗口余量不足时跳过点击、保留一个待执行帧至后续刷新，不补发旧帧；单次超限停止该文档的 Python。</div>
        <div class="secondary">时间保护为协作式检查；清理另有约 50 ms 尽力预算。输出按 Python 文本的 UTF-8 字节计量。</div>
        ${this.pythonSettings_?.commandLineOverride ? html`
          <div class="secondary">启动命令行含 Python 配置，可能覆盖此处设置。</div>
        ` : ''}
      </div>
    </div>
    <div class="cr-row python-actions">
      <cr-button id="savePythonSettings"
          ?disabled="${!this.pythonSettings_?.supported || this.pythonSaving_}"
          @click="${this.onSavePythonSettingsClick_}">保存设置</cr-button>
      ${this.pythonSettings_?.restartRequired ? html`
        <span class="flex secondary">重启后应用已保存设置</span>
        <cr-button id="relaunchPython" @click="${this.onPythonRelaunchClick_}">重新启动</cr-button>
      ` : ''}
    </div>
    <div id="pythonMessage" class="cr-padded-text secondary python-message"
        role="status">${this.pythonMessage_}</div>
  </settings-section>
  ${this.shouldShowRelaunchDialog ? html`
    <relaunch-confirmation-dialog .restartType="${RestartType.RELAUNCH}"
        @close="${this.onRelaunchDialogClose}"></relaunch-confirmation-dialog>
  ` : ''}
  <settings-section page-title="Hello chromium"
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
