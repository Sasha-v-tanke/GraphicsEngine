import { StrictMode, useCallback, useEffect, useMemo, useState } from 'react'
import { createRoot } from 'react-dom/client'
import { createApi, type PerformanceHubApi } from './api/client'
import { useHealth } from './useHealth'
import { DateRangePicker, Pagination, SearchField } from './components/controls'
import { EmptyState, ErrorState, LoadingState, Notice } from './components/states'
import { normalizeRoute, routes } from './routes'
import './styles.css'

function navigate(path: string) {
  window.history.pushState({}, '', path)
  window.dispatchEvent(new PopStateEvent('popstate'))
}

function App({ api }: { api?: PerformanceHubApi }) {
  const defaultApi = useMemo(() => createApi(), [])
  const activeApi = api ?? defaultApi
  const [route, setRoute] = useState(() => normalizeRoute(window.location.pathname))
  const [urlSearch, setUrlSearch] = useState(() => window.location.search)
  const { health, error: healthError, loading, refresh: loadHealth } = useHealth(activeApi)

  useEffect(() => {
    const canonicalize = () => {
      const normalized = normalizeRoute(window.location.pathname)
      if (normalized !== window.location.pathname) window.history.replaceState({}, '', normalized)
      const params = new URLSearchParams(window.location.search)
      const range = params.get('range')
      if (normalized === '/runs' && range !== null && !['7d', '30d', 'all'].includes(range)) {
        params.set('range', '7d')
        window.history.replaceState({}, '', `${normalized}?${params}`)
      }
      return normalized
    }
    const listener = () => {
      setRoute(canonicalize())
      setUrlSearch(window.location.search)
    }
    canonicalize()
    window.addEventListener('popstate', listener)
    return () => window.removeEventListener('popstate', listener)
  }, [])

  const replaceSearch = useCallback((params: URLSearchParams) => {
    window.history.replaceState({}, '', `${window.location.pathname}${params.size ? `?${params}` : ''}`)
    window.dispatchEvent(new PopStateEvent('popstate'))
  }, [])

  const setBenchmarkQuery = useCallback((query: string) => {
    const params = new URLSearchParams(window.location.search)
    if (query) params.set('q', query); else params.delete('q')
    replaceSearch(params)
  }, [replaceSearch])

  const setRunRange = useCallback((range: string) => {
    const params = new URLSearchParams(window.location.search)
    params.set('range', range)
    replaceSearch(params)
  }, [replaceSearch])

  const currentRoute = routes.find((item) => item.path === route)
  return <div className="app-shell">
    <a className="skip-link" href="#content">Skip to content</a>
    <aside>
      <h1>Performance Hub</h1>
      <nav aria-label="Main navigation"><ul>{routes.map((item) => <li key={item.path}>{item.enabled
        ? <a aria-current={route === item.path ? 'page' : undefined} href={item.path} onClick={(event) => { event.preventDefault(); navigate(item.path) }}>{item.label}</a>
        : <span aria-disabled="true" title="Available in a later stage">{item.label}</span>
      }</li>)}</ul></nav>
      <button className="status-link" type="button" onClick={() => navigate('/benchmarks')}>Backend: {loading ? 'checking' : health?.status ?? 'unavailable'}</button>
    </aside>
    <main id="content">
      <header><p className="eyebrow">Performance measurements</p><h2>{currentRoute?.label}</h2></header>
      {route === '/benchmarks' && <Benchmarks query={new URLSearchParams(urlSearch).get('q') ?? ''} onQueryChange={setBenchmarkQuery} />}
      {route === '/run-presets' && <RunPresets />}
      {route === '/runs' && <Runs range={new URLSearchParams(urlSearch).get('range') ?? '7d'} onRangeChange={setRunRange} />}
      <section className="health"><h2>Service status</h2>{loading && <LoadingState label="Checking /health…" />}{healthError && <ErrorState error={healthError} retry={loadHealth} />}{health && <p role="status"><strong>{health.status}</strong>{health.version ? ` · ${health.version}` : ''}{health.detail ? ` · ${health.detail}` : ''}</p>}</section>
    </main>
  </div>
}

function Benchmarks({ query, onQueryChange }: { query: string; onQueryChange(query: string): void }) {
  return <section><SearchField value={query} onChange={onQueryChange} label="Search benchmark catalog" /><EmptyState title="Catalog is not connected" detail="The backend catalog will be available in a later stage." /></section>
}

function RunPresets() { return <section><Notice>Preset editing is not available until the versioned Run Presets API is integrated.</Notice><EmptyState title="No presets yet" detail="Create presets after the backend contract is available." /></section> }

function Runs({ range, onRangeChange }: { range: string; onRangeChange(range: string): void }) {
  range = ['7d', '30d', 'all'].includes(range) ? range : '7d'
  const [page, setPage] = useState(1)
  const title = range === 'all' ? 'No recorded runs' : 'No runs in this range'
  return <section><DateRangePicker value={range} onChange={onRangeChange} /><EmptyState title={title} detail="Completed benchmark runs will appear here." /><Pagination page={page} pageCount={1} onPage={setPage} /></section>
}

const rootElement = document.getElementById('root')
if (rootElement) createRoot(rootElement).render(<StrictMode><App /></StrictMode>)
export { App }
