import { act } from 'react'
import { createRoot, type Root } from 'react-dom/client'
import { afterEach, expect, it, vi } from 'vitest'
import { createApi, type ServiceHealth, type PerformanceHubApi } from '../api/client'
import { useHealth } from '../useHealth'
import { CheckboxTree } from '../components/controls'
import { DataTable } from '../components/DataTable'
import { App } from '../main'

let root: Root
let host: HTMLDivElement
async function mount(element: React.ReactNode) {
  host = document.createElement('div')
  document.body.append(host)
  root = createRoot(host)
  await act(async () => root.render(element))
}
afterEach(async () => {
  if (root) await act(async () => root.unmount())
  document.body.replaceChildren()
  vi.restoreAllMocks()
})

it.each([{ status: ['ok'] }, { status: 'ok', version: {} }, { status: 'ok', detail: 12 }, { status: 'unknown' }])('rejects malformed health DTO %j', async (payload) => {
  vi.spyOn(globalThis, 'fetch').mockResolvedValue(new Response(JSON.stringify(payload)))
  await expect(createApi().health()).rejects.toThrow('does not match')
})

it('ignores superseded health responses, errors and completion even when adapters ignore abort', async () => {
  const pending: { resolve(value: ServiceHealth): void; reject(error: Error): void; signal?: AbortSignal }[] = []
  const api: PerformanceHubApi = { health: (signal) => new Promise((resolve, reject) => pending.push({ resolve, reject, signal })) }
  function Harness({ client }: { client: PerformanceHubApi }) {
    const state = useHealth(client)
    return <><button onClick={state.refresh}>Refresh</button><output>{state.loading ? 'loading' : state.error?.message ?? state.health?.status}</output></>
  }
  await mount(<Harness client={api} />)
  await act(async () => host.querySelector('button')!.click())
  expect(pending[0].signal?.aborted).toBe(true)
  await act(async () => pending[0].resolve({ status: 'degraded' }))
  expect(host.querySelector('output')!.textContent).toBe('loading')
  await act(async () => pending[1].resolve({ status: 'ok' }))
  expect(host.querySelector('output')!.textContent).toBe('ok')
  await act(async () => host.querySelector('button')!.click())
  const replacement: PerformanceHubApi = { health: vi.fn().mockResolvedValue({ status: 'unavailable' }) }
  await act(async () => root.render(<Harness client={replacement} />))
  await act(async () => pending[2].reject(new Error('stale error')))
  expect(host.querySelector('output')!.textContent).toBe('unavailable')
})

it('uses actual leaf IDs for Select All after the tree changes', async () => {
  const change = vi.fn()
  await mount(<CheckboxTree items={[{ id: 'new', label: 'New' }]} selected={new Set(['old'])} onChange={change} />)
  expect(host.querySelector('button')!.textContent).toBe('Select all')
  await act(async () => host.querySelector('button')!.click())
  expect(change).toHaveBeenCalledWith(new Set(['new']))
})

it('paginates table rows and handles a shrinking dataset and empty state', async () => {
  const columns = [{ id: 'name', header: 'Name', cell: (row: string) => row }]
  await mount(<DataTable caption="Runs" rows={['first', 'second', 'third']} columns={columns} rowKey={(row) => row} pageSize={2} />)
  expect(host.querySelector('tbody')!.textContent).toBe('firstsecond')
  await act(async () => (host.querySelectorAll('button')[1] as HTMLButtonElement).click())
  expect(host.querySelector('tbody')!.textContent).toBe('third')
  await act(async () => root.render(<DataTable caption="Runs" rows={['first']} columns={columns} rowKey={(row) => row} pageSize={2} />))
  expect(host.querySelector('tbody')!.textContent).toBe('first')
  await act(async () => root.render(<DataTable caption="Runs" rows={[]} columns={columns} rowKey={(row) => row} />))
  expect(host.querySelector('tbody')!.textContent).toBe('No records')
})

it('normalizes invalid URL ranges on mount and history navigation', async () => {
  window.history.replaceState({}, '', '/runs?range=invalid&keep=yes')
  await mount(<App api={{ health: vi.fn().mockResolvedValue({ status: 'ok' }) }} />)
  expect((host.querySelector('select') as HTMLSelectElement).value).toBe('7d')
  expect(window.location.search).toBe('?range=7d&keep=yes')
  await act(async () => {
    window.history.pushState({}, '', '/runs?range=bad')
    window.dispatchEvent(new PopStateEvent('popstate'))
  })
  expect((host.querySelector('select') as HTMLSelectElement).value).toBe('7d')
  expect(window.location.search).toBe('?range=7d')
})
