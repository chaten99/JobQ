import { expect, test } from '@playwright/test';
import { spawn, type ChildProcess } from 'node:child_process';
import { once } from 'node:events';
import { resolve } from 'node:path';

function start(file: string, args: string[], env = process.env) {
  return spawn(file, args, { env, windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'] });
}

function listening(child: ChildProcess, pattern: RegExp) {
  return new Promise<string>((resolve, reject) => {
    let output = '';
    const timer = setTimeout(() => finish(new Error('Startup timed out: ' + output)), 15_000);
    const finish = (error?: Error, value = '') => {
      clearTimeout(timer);
      child.stdout?.off('data', collect);
      child.stderr?.off('data', collect);
      child.off('error', finish);
      child.off('exit', exited);
      if (error) reject(error);
      else resolve(value);
    };
    const exited = () => finish(new Error('Process exited during startup: ' + output));
    const collect = (chunk: Buffer) => {
      output = (output + chunk.toString()).slice(-8192);
      const match = pattern.exec(output);
      if (match) finish(undefined, match[1]);
    };
    child.stdout?.on('data', collect);
    child.stderr?.on('data', collect);
    child.once('error', finish);
    child.once('exit', exited);
  });
}

async function stop(child?: ChildProcess) {
  if (!child?.pid || child.exitCode !== null || child.signalCode !== null) return;
  const exited = once(child, 'exit');
  child.kill();
  let timer: ReturnType<typeof setTimeout> | undefined;
  try {
    await Promise.race([exited, new Promise((_, reject) => {
      timer = setTimeout(() => reject(new Error('Test process failed to exit')), 5000);
    })]);
  } finally { clearTimeout(timer); }
}

test('real job flows through the Vite proxy, WebSocket, and metrics dashboard', async ({ page, request }) => {
  const host = process.env.JOBQ_E2E_HOST;
  if (!host) throw new Error('Set JOBQ_E2E_HOST to the built jobq_rest_test_host executable.');
  let backend: ChildProcess | undefined;
  let vite: ChildProcess | undefined;
  try {
    backend = start(resolve(host), ['--port', '0']);
    const port = await listening(backend, /JobQ HTTP server ready on 127\.0\.0\.1:(\d+)/);
    vite = start(process.execPath, ['node_modules/vite/bin/vite.js', '--host', '127.0.0.1', '--port', '0'], {
      ...process.env,
      JOBQ_PROXY_TARGET: 'http://127.0.0.1:' + port,
      VITE_JOBQ_API_BASE_URL: '',
      VITE_JOBQ_WS_URL: '',
      NO_COLOR: '1',
    });
    const base = await listening(vite, /Local:\s+(http:\/\/127\.0\.0\.1:\d+)/);
    let sockets = 0;
    let closed = 0;
    page.on('websocket', socket => {
      if (!socket.url().endsWith('/api/v1/events')) return;
      sockets++;
      socket.on('close', () => closed++);
    });
    await page.goto(base);
    await expect(page.getByText('Events connected')).toBeVisible();
    await expect(page.getByRole('heading', { name: 'Runtime' })).toBeVisible();
    await expect(page.getByRole('heading', { name: 'Worker pool' })).toBeVisible();
    await expect(page.getByText('Ready to run')).toBeVisible();
    await page.getByRole('link', { name: 'Metrics' }).click();
    await expect(page.getByText('No execution attempts have finished.')).toBeVisible();
    const response = await request.post(base + '/api/v1/jobs', { data: { type: 'ECHO', payload: 'Browser metric test' } });
    expect(response.status()).toBe(201);
    const { data: { id } } = await response.json() as { data: { id: string } };
    await page.getByRole('button', { name: 'Refresh snapshots' }).click();
    await expect(page.getByText('Submitted jobs').locator('..')).toContainText('1');
    await expect(page.getByText('Finished executions').locator('..')).toContainText('1');
    await expect(page.getByRole('img', { name: 'Observed finished execution attempts per second' })).toBeVisible();
    await page.getByRole('link', { name: 'Jobs', exact: true }).click();
    await expect(page.getByRole('row').filter({ hasText: '#' + id })).toContainText('completed');
    await page.getByRole('link', { name: 'Events', exact: true }).click();
    await expect(page.getByRole('link', { name: '#' + id })).toHaveCount(2);
    await expect(page.getByText('completed', { exact: true })).toBeVisible();
    expect(sockets).toBe(1);
    await stop(backend);
    await page.getByRole('link', { name: 'Metrics' }).click();
    await expect(page.getByRole('alert')).toBeVisible();
    await expect.poll(() => closed).toBeGreaterThanOrEqual(1);
    await page.close();
  } finally {
    await Promise.all([stop(vite), stop(backend)]);
  }
});
