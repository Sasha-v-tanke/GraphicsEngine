import { StrictMode, useCallback, useEffect, useMemo, useState } from 'react'
import { createRoot } from 'react-dom/client'
import { createApi, type PerformanceHubApi, type ServiceHealth } from './api/client'
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
  const [health, setHealth] = useState<ServiceHealth>()
  const [healthError, setHealthError] = useState<Error>()
  const [loading, setLoading] = useState(true)

  const loadHealth = useCallback(() => {
    setLoading(true)
    setHealthError(undefined)
    activeApi.health()
      .then(setHealth)
      .catch((error: unknown) => setHealthError(error instanceof Error ? error : new Error('Health request failed')))
      .finally(() => setLoading(false))
  }, [activeApi])

  useEffect(() => {
    const canonicalize = () => {
      const normalized = normalizeRoute(window.location.pathname)
      if (normalized !== window.location.pathname) window.history.replaceState({}, '', normalized)
      return normalized
    }
    const listener = () => setRoute(canonicalize())
    canonicalize()
    window.addEventListener('popstate', listener)
    return () => window.removeEventListener('popstate', listener)
  }, [])

  useEffect(() => {
    let cancelled = false
    activeApi.health()
      .then((value) => { if (!cancelled) setHealth(value) })
      .catch((error: unknown) => { if (!cancelled) setHealthError(error instanceof Error ? error : new Error('Health request failed')) })
      .finally(() => { if (!cancelled) setLoading(false) })
    return () => { cancelled = true }
  }, [activeApi])

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
      {route === '/benchmarks' && <Benchmarks />}
      {route === '/run-presets' && <RunPresets />}
      {route === '/runs' && <Runs />}
      <section className="health"><h2>Service status</h2>{loading && <LoadingState label="Checking /health…" />}{healthError && <ErrorState error={healthError} retry={loadHealth} />}{health && <p role="status"><strong>{health.status}</strong>{health.version ? ` · ${health.version}` : ''}{health.detail ? ` · ${health.detail}` : ''}</p>}</section>
    </main>
  </div>
}

function Benchmarks() {
  const query = new URLSearchParams(window.location.search).get('q') ?? ''
  const [search, setSearch] = useState(query)
  useEffect(() => {
    const params = new URLSearchParams(window.location.search)
    if (search) params.set('q', search); else params.delete('q')
    window.history.replaceState({}, '', `/benchmarks${params.size ? `?${params}` : ''}`)
  }, [search])
  return <section><SearchField value={search} onChange={setSearch} label="Search benchmark catalog" /><EmptyState title="Catalog is not connected" detail="The backend catalog will be available in a later stage." /></section>
}

function RunPresets() { return <section><Notice>Preset editing is not available until the versioned Run Presets API is integrated.</Notice><EmptyState title="No presets yet" detail="Create presets after the backend contract is available." /></section> }

function Runs() {
  const [range, setRange] = useState(new URLSearchParams(window.location.search).get('range') ?? '7d')
  const [page, setPage] = useState(1)
  useEffect(() => { window.history.replaceState({}, '', `/runs?range=${range}`) }, [range])
  const title = range === 'all' ? 'No recorded runs' : 'No runs in this range'
  return <section><DateRangePicker value={range} onChange={setRange} /><EmptyState title={title} detail="Completed benchmark runs will appear here." /><Pagination page={page} pageCount={1} onPage={setPage} /></section>
}

const rootElement = document.getElementById('root')
if (rootElement) createRoot(rootElement).render(<StrictMode><App /></StrictMode>)
export { App }
