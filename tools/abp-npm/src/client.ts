// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import { request } from "./http.js";
import type {
  ActionResponse,
  BrowserStatus,
  SessionData,
  ShutdownOptions,
  Tab,
  CreateTabOptions,
  CreatedTab,
  ActivateResult,
  NavigateOptions,
  ClickOptions,
  MoveOptions,
  ScrollOptions,
  DragOptions,
  TypeOptions,
  KeyOptions,
  ScreenshotOptions,
  ExecuteOptions,
  ExecuteResult,
  TextOptions,
  TextResult,
  WaitOptions,
  DialogInfo,
  AcceptDialogOptions,
  ExecutionState,
  SetExecutionOptions,
  Download,
  ListDownloadsOptions,
  FileChooserOptions,
  Session,
  HistoryAction,
  HistoryEvent,
  ActionRequest,
} from "./types.js";

class BrowserAPI {
  constructor(private baseUrl: string) {}

  async status(): Promise<BrowserStatus> {
    const res = await request<BrowserStatus>(`${this.baseUrl}/browser/status`);
    return res.data;
  }

  async sessionData(): Promise<SessionData> {
    const res = await request<SessionData>(`${this.baseUrl}/browser/session-data`);
    return res.data;
  }

  async shutdown(options?: ShutdownOptions): Promise<void> {
    await request(`${this.baseUrl}/browser/shutdown`, {
      method: "POST",
      body: options || {},
    });
  }
}

class TabsAPI {
  constructor(private baseUrl: string) {}

  async list(): Promise<Tab[]> {
    const res = await request<Tab[]>(`${this.baseUrl}/tabs`);
    return res.data;
  }

  async get(tabId: string): Promise<Tab> {
    const res = await request<Tab>(`${this.baseUrl}/tabs/${tabId}`);
    return res.data;
  }

  async create(options?: CreateTabOptions): Promise<CreatedTab> {
    const res = await request<CreatedTab>(`${this.baseUrl}/tabs`, {
      method: "POST",
      body: options || {},
    });
    return res.data;
  }

  async close(tabId: string): Promise<void> {
    await request(`${this.baseUrl}/tabs/${tabId}`, { method: "DELETE" });
  }

  async activate(tabId: string): Promise<ActivateResult> {
    const res = await request<ActivateResult>(
      `${this.baseUrl}/tabs/${tabId}/activate`,
      { method: "POST", body: {} },
    );
    return res.data;
  }

  async stop(tabId: string): Promise<{ status: string; tab_id: string }> {
    const res = await request<{ status: string; tab_id: string }>(
      `${this.baseUrl}/tabs/${tabId}/stop`,
      { method: "POST", body: {} },
    );
    return res.data;
  }

  async navigate(tabId: string, options: NavigateOptions): Promise<ActionResponse> {
    const res = await request<ActionResponse>(
      `${this.baseUrl}/tabs/${tabId}/navigate`,
      { method: "POST", body: options },
    );
    return res.data;
  }

  async reload(tabId: string, options?: ActionRequest): Promise<ActionResponse> {
    const res = await request<ActionResponse>(
      `${this.baseUrl}/tabs/${tabId}/reload`,
      { method: "POST", body: options || {} },
    );
    return res.data;
  }

  async back(tabId: string, options?: ActionRequest): Promise<ActionResponse> {
    const res = await request<ActionResponse>(
      `${this.baseUrl}/tabs/${tabId}/back`,
      { method: "POST", body: options || {} },
    );
    return res.data;
  }

  async forward(tabId: string, options?: ActionRequest): Promise<ActionResponse> {
    const res = await request<ActionResponse>(
      `${this.baseUrl}/tabs/${tabId}/forward`,
      { method: "POST", body: options || {} },
    );
    return res.data;
  }

  async click(tabId: string, options: ClickOptions): Promise<ActionResponse> {
    const res = await request<ActionResponse>(
      `${this.baseUrl}/tabs/${tabId}/click`,
      { method: "POST", body: options },
    );
    return res.data;
  }

  async move(tabId: string, options: MoveOptions): Promise<ActionResponse> {
    const res = await request<ActionResponse>(
      `${this.baseUrl}/tabs/${tabId}/move`,
      { method: "POST", body: options },
    );
    return res.data;
  }

  async scroll(tabId: string, options: ScrollOptions): Promise<ActionResponse> {
    const res = await request<ActionResponse>(
      `${this.baseUrl}/tabs/${tabId}/scroll`,
      { method: "POST", body: options },
    );
    return res.data;
  }

  async drag(tabId: string, options: DragOptions): Promise<ActionResponse> {
    const res = await request<ActionResponse>(
      `${this.baseUrl}/tabs/${tabId}/drag`,
      { method: "POST", body: options },
    );
    return res.data;
  }

  async type(tabId: string, options: TypeOptions): Promise<ActionResponse> {
    const res = await request<ActionResponse>(
      `${this.baseUrl}/tabs/${tabId}/type`,
      { method: "POST", body: options },
    );
    return res.data;
  }

  async keyPress(tabId: string, options: KeyOptions): Promise<ActionResponse> {
    const res = await request<ActionResponse>(
      `${this.baseUrl}/tabs/${tabId}/keyboard/press`,
      { method: "POST", body: options },
    );
    return res.data;
  }

  async keyDown(tabId: string, options: KeyOptions): Promise<ActionResponse> {
    const res = await request<ActionResponse>(
      `${this.baseUrl}/tabs/${tabId}/keyboard/down`,
      { method: "POST", body: options },
    );
    return res.data;
  }

  async keyUp(tabId: string, options: KeyOptions): Promise<ActionResponse> {
    const res = await request<ActionResponse>(
      `${this.baseUrl}/tabs/${tabId}/keyboard/up`,
      { method: "POST", body: options },
    );
    return res.data;
  }

  async screenshot(tabId: string, options?: ScreenshotOptions): Promise<ActionResponse> {
    const res = await request<ActionResponse>(
      `${this.baseUrl}/tabs/${tabId}/screenshot`,
      { method: "POST", body: { screenshot: options || {} } },
    );
    return res.data;
  }

  async screenshotBinary(tabId: string, options?: { disable_markup?: string[] }): Promise<Buffer> {
    const query = options?.disable_markup?.length
      ? `?disable_markup=${options.disable_markup.join(",")}`
      : "";
    const res = await request<Buffer>(
      `${this.baseUrl}/tabs/${tabId}/screenshot${query}`,
    );
    return res.data;
  }

  async execute(tabId: string, options: ExecuteOptions): Promise<ActionResponse<ExecuteResult>> {
    const res = await request<ActionResponse<ExecuteResult>>(
      `${this.baseUrl}/tabs/${tabId}/execute`,
      { method: "POST", body: options },
    );
    return res.data;
  }

  async text(tabId: string, options?: TextOptions): Promise<TextResult> {
    const res = await request<TextResult>(
      `${this.baseUrl}/tabs/${tabId}/text`,
      { method: "POST", body: options || {} },
    );
    return res.data;
  }

  async wait(tabId: string, options: WaitOptions): Promise<ActionResponse> {
    const res = await request<ActionResponse>(
      `${this.baseUrl}/tabs/${tabId}/wait`,
      { method: "POST", body: options },
    );
    return res.data;
  }

  async dialog(tabId: string): Promise<DialogInfo> {
    const res = await request<DialogInfo>(
      `${this.baseUrl}/tabs/${tabId}/dialog`,
    );
    return res.data;
  }

  async dialogAccept(tabId: string, options?: AcceptDialogOptions): Promise<{ success: boolean }> {
    const res = await request<{ success: boolean }>(
      `${this.baseUrl}/tabs/${tabId}/dialog/accept`,
      { method: "POST", body: options || {} },
    );
    return res.data;
  }

  async dialogDismiss(tabId: string): Promise<{ success: boolean }> {
    const res = await request<{ success: boolean }>(
      `${this.baseUrl}/tabs/${tabId}/dialog/dismiss`,
      { method: "POST", body: {} },
    );
    return res.data;
  }

  async execution(tabId: string): Promise<ExecutionState> {
    const res = await request<ExecutionState>(
      `${this.baseUrl}/tabs/${tabId}/execution`,
    );
    return res.data;
  }

  async setExecution(tabId: string, options: SetExecutionOptions): Promise<ExecutionState> {
    const res = await request<ExecutionState>(
      `${this.baseUrl}/tabs/${tabId}/execution`,
      { method: "POST", body: options },
    );
    return res.data;
  }
}

class DownloadsAPI {
  constructor(private baseUrl: string) {}

  async list(options?: ListDownloadsOptions): Promise<{ downloads: Download[] }> {
    const params = new URLSearchParams();
    if (options?.state) params.set("state", options.state);
    if (options?.limit) params.set("limit", String(options.limit));
    const query = params.toString() ? `?${params.toString()}` : "";
    const res = await request<{ downloads: Download[] }>(
      `${this.baseUrl}/downloads${query}`,
    );
    return res.data;
  }

  async get(downloadId: string): Promise<Download> {
    const res = await request<Download>(
      `${this.baseUrl}/downloads/${downloadId}`,
    );
    return res.data;
  }

  async cancel(downloadId: string): Promise<{ success: boolean; message: string }> {
    const res = await request<{ success: boolean; message: string }>(
      `${this.baseUrl}/downloads/${downloadId}/cancel`,
      { method: "POST", body: {} },
    );
    return res.data;
  }
}

class FileChooserAPI {
  constructor(private baseUrl: string) {}

  async provide(chooserId: string, options: FileChooserOptions): Promise<{ success: boolean; cancelled?: boolean }> {
    const res = await request<{ success: boolean; cancelled?: boolean }>(
      `${this.baseUrl}/file-chooser/${chooserId}`,
      { method: "POST", body: options },
    );
    return res.data;
  }
}

class HistoryAPI {
  constructor(private baseUrl: string) {}

  async sessions(): Promise<Session[]> {
    const res = await request<Session[]>(`${this.baseUrl}/history/sessions`);
    return res.data;
  }

  async currentSession(): Promise<Session> {
    const res = await request<Session>(`${this.baseUrl}/history/sessions/current`);
    return res.data;
  }

  async session(sessionId: string): Promise<Session> {
    const res = await request<Session>(`${this.baseUrl}/history/sessions/${sessionId}`);
    return res.data;
  }

  async exportSession(sessionId: string): Promise<unknown> {
    const res = await request(`${this.baseUrl}/history/sessions/${sessionId}/export`);
    return res.data;
  }

  async actions(): Promise<HistoryAction[]> {
    const res = await request<HistoryAction[]>(`${this.baseUrl}/history/actions`);
    return res.data;
  }

  async action(actionId: string): Promise<HistoryAction> {
    const res = await request<HistoryAction>(`${this.baseUrl}/history/actions/${actionId}`);
    return res.data;
  }

  async actionScreenshot(actionId: string): Promise<Buffer> {
    const res = await request<Buffer>(`${this.baseUrl}/history/actions/${actionId}/screenshot`);
    return res.data;
  }

  async deleteActions(): Promise<void> {
    await request(`${this.baseUrl}/history/actions`, { method: "DELETE" });
  }

  async events(): Promise<HistoryEvent[]> {
    const res = await request<HistoryEvent[]>(`${this.baseUrl}/history/events`);
    return res.data;
  }

  async event(eventId: string): Promise<HistoryEvent> {
    const res = await request<HistoryEvent>(`${this.baseUrl}/history/events/${eventId}`);
    return res.data;
  }

  async deleteEvents(): Promise<void> {
    await request(`${this.baseUrl}/history/events`, { method: "DELETE" });
  }

  async deleteAll(): Promise<void> {
    await request(`${this.baseUrl}/history`, { method: "DELETE" });
  }
}

export class ABPClient {
  readonly browser: BrowserAPI;
  readonly tabs: TabsAPI;
  readonly downloads: DownloadsAPI;
  readonly fileChooser: FileChooserAPI;
  readonly history: HistoryAPI;

  constructor(baseUrl: string = "http://localhost:8222/api/v1") {
    const url = baseUrl.replace(/\/+$/, "");
    this.browser = new BrowserAPI(url);
    this.tabs = new TabsAPI(url);
    this.downloads = new DownloadsAPI(url);
    this.fileChooser = new FileChooserAPI(url);
    this.history = new HistoryAPI(url);
  }
}
