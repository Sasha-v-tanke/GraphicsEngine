import { act } from 'react'
import { createRoot, type Root } from 'react-dom/client'
import { afterEach, describe, expect, it, vi } from 'vitest'
import { App } from '../main'
import type { PerformanceHubApi } from '../api/client'
import { normalizeRoute } from '../routes'

const roots: Root[] = []

function render(app: React.ReactNode) {
  const host = document.createElement('div')
  document.body.append(host)
  const root = createRoot(host)
  roots.push(root)
  root.render(app)
  return { host, root }
}

afterEach(() => {
  for (const root of roots.splice(0)) root.unmount()
  document.body.replaceChildren()
  vi.restoreAllMocks()
})

describe('Performance Hub shell', () => {
  it('does not repeat the health request after its result rerenders the app', async () => {
    const fetchMock = vi.spyOn(globalThis, 'fetch').mockResolvedValue(new Response(JSON.stringify({ status: 'ok' }), { status: 200 }))
    const { root } = render(<App />)
    await act(async () => { root.render(<App />) })
    expect(fetchMock).toHaveBeenCalledTimes(1)
  })

  it('renders an API error and retries health on demand', async () => {
    const api: PerformanceHubApi = { health: vi.fn().mockRejectedValueOnce(new Error('offline')).mockResolvedValueOnce({ status: 'ok' }) }
    const { host } = render(<App api={api} />)
    await act(async () => {})
    expect(host.querySelector('[role="alert"]')?.textContent).toContain('offline')
    await act(async () => { (host.querySelector('[role="alert"] button') as HTMLButtonElement).click() })
    expect(api.health).toHaveBeenCalledTimes(2)
    expect(host.querySelector('[role="status"]')?.textContent).toContain('ok')
  })

  it('keeps keyboard-visible navigation limited to enabled routes', async () => {
    const api: PerformanceHubApi = { health: vi.fn().mockResolvedValue({ status: 'ok' }) }
    const { host } = render(<App api={api} />)
    await act(async () => {})
    const skip = host.querySelector('.skip-link') as HTMLAnchorElement
    skip.focus()
    expect(document.activeElement).toBe(skip)
    expect(host.querySelector('a[href="/compare"]')).toBeNull()
    expect(host.querySelector('[aria-disabled="true"]')?.textContent).toContain('Compare')
  })

  it('redirects disabled and unknown direct links to benchmarks', async () => {
    expect(normalizeRoute('/compare')).toBe('/benchmarks')
    expect(normalizeRoute('/profiler')).toBe('/benchmarks')
    expect(normalizeRoute('/unknown')).toBe('/benchmarks')
    expect(normalizeRoute('/runs')).toBe('/runs')
    window.history.replaceState({}, '', '/compare')
    const api: PerformanceHubApi = { health: vi.fn().mockResolvedValue({ status: 'ok' }) }
    const { host } = render(<App api={api} />)
    await act(async () => {})
    expect(window.location.pathname).toBe('/benchmarks')
    expect(host.querySelector('main h2')?.textContent).toBe('Benchmarks')
  })

  it('synchronizes benchmark and run filters after browser history navigation', async () => {
    window.history.replaceState({}, '', '/benchmarks?q=renderer')
    const api: PerformanceHubApi = { health: vi.fn().mockResolvedValue({ status: 'ok' }) }
    const { host } = render(<App api={api} />)
    await act(async () => {})
    expect((host.querySelector('input[type="search"]') as HTMLInputElement).value).toBe('renderer')
    await act(async () => {
      window.history.pushState({}, '', '/benchmarks?q=ecs')
      window.dispatchEvent(new PopStateEvent('popstate'))
    })
    expect((host.querySelector('input[type="search"]') as HTMLInputElement).value).toBe('ecs')
    await act(async () => {
      window.history.pushState({}, '', '/runs?range=all')
      window.dispatchEvent(new PopStateEvent('popstate'))
    })
    expect((host.querySelector('select') as HTMLSelectElement).value).toBe('all')
  })
})
