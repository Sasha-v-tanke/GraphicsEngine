import { describe, expect, it, vi } from 'vitest'
import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { App } from '../main'
import type { PerformanceHubApi } from '../api/client'

const api: PerformanceHubApi = { health: vi.fn().mockResolvedValue({ status: 'ok', version: 'v1' }) }

describe('Performance Hub shell', () => {
  it('renders navigation and disables future pages', async () => {
    const host = document.createElement('div')
    document.body.append(host)
    await act(async () => { createRoot(host).render(<App api={api} />) })
    expect(host.querySelector('a[href="/benchmarks"]')).not.toBeNull()
    expect(host.querySelector('[aria-disabled="true"]')?.textContent).toContain('Compare')
  })

  it('uses the current URL for a direct benchmark link', async () => {
    window.history.replaceState({}, '', '/benchmarks?q=renderer')
    const host = document.createElement('div')
    document.body.append(host)
    await act(async () => { createRoot(host).render(<App api={api} />) })
    expect((host.querySelector('input[type="search"]') as HTMLInputElement).value).toBe('renderer')
  })
})
