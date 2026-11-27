import { StrictMode, useEffect, useMemo, useState } from 'react'
import { createRoot } from 'react-dom/client'
import { createApi, type PerformanceHubApi, type ServiceHealth } from './api/client'
import { CheckboxTree, DateRangePicker, Pagination, SearchField, type TreeItem } from './components/controls'
import { EmptyState, ErrorState, LoadingState, Notice } from './components/states'
import './styles.css'

type Route = { path: string; label: string; enabled: boolean }
const routes: Route[] = [
  { path: '/benchmarks', label: 'Benchmarks', enabled: true }, { path: '/run-presets', label: 'Run Presets', enabled: true },
  { path: '/runs', label: 'Runs', enabled: true }, { path: '/compare', label: 'Compare', enabled: false },
  { path: '/metrics', label: 'Metrics', enabled: false }, { path: '/dashboards', label: 'Dashboards', enabled: false }, { path: '/profiler', label: 'Profiler', enabled: false },
]
const tree: TreeItem[] = [{ id: 'engine', label: 'Engine', children: [{ id: 'frame', label: 'Frame loop' }, { id: 'renderer', label: 'Renderer' }] }]

function normalizeRoute(pathname: string) { return routes.some((route) => route.path === pathname) ? pathname : '/benchmarks' }
function navigate(path: string) { window.history.pushState({}, '', path); window.dispatchEvent(new PopStateEvent('popstate')) }

function App({ api = createApi() }: { api?: PerformanceHubApi }) {
  const [route, setRoute] = useState(() => normalizeRoute(window.location.pathname))
  const [health, setHealth] = useState<ServiceHealth>()
  const [healthError, setHealthError] = useState<Error>()
  const [loading, setLoading] = useState(true)
  useEffect(() => { const listener = () => setRoute(normalizeRoute(window.location.pathname)); window.addEventListener('popstate', listener); return () => window.removeEventListener('popstate', listener) }, [])
  const refreshHealth = () => { setLoading(true); setHealthError(undefined); api.health().then(setHealth).catch((error: unknown) => setHealthError(error instanceof Error ? error : new Error('Health request failed'))).finally(() => setLoading(false)) }
  useEffect(() => {
    let cancelled = false
    api.health().then((value) => { if (!cancelled) setHealth(value) }).catch((error: unknown) => { if (!cancelled) setHealthError(error instanceof Error ? error : new Error('Health request failed')) }).finally(() => { if (!cancelled) setLoading(false) })
    return () => { cancelled = true }
  }, [api])
  return <div className="app-shell"><a className="skip-link" href="#content">Skip to content</a><aside><h1>Performance Hub</h1><nav aria-label="Main navigation"><ul>{routes.map((item) => <li key={item.path}>{item.enabled ? <a aria-current={route === item.path ? 'page' : undefined} href={item.path} onClick={(event) => { event.preventDefault(); navigate(item.path) }}>{item.label}</a> : <span aria-disabled="true" title="Available in a later stage">{item.label}</span>}</li>)}</ul></nav><button className="status-link" type="button" onClick={() => navigate('/benchmarks')}>Backend: {loading ? 'checking' : health?.status ?? 'unavailable'}</button></aside><main id="content"><header><p className="eyebrow">Performance measurements</p><h2>{routes.find((item) => item.path === route)?.label}</h2></header>{route === '/benchmarks' && <Benchmarks />}{route === '/run-presets' && <RunPresets />}{route === '/runs' && <Runs />}<section className="health"><h2>Service status</h2>{loading && <LoadingState label="Checking /health…" />}{healthError && <ErrorState error={healthError} retry={refreshHealth} />}{health && <p role="status"><strong>{health.status}</strong>{health.version ? ` · ${health.version}` : ''}{health.detail ? ` · ${health.detail}` : ''}</p>}</section></main></div>
}

function Benchmarks() { const query = new URLSearchParams(window.location.search).get('q') ?? ''; const [search, setSearch] = useState(query); const [selected, setSelected] = useState(new Set<string>()); useEffect(() => { const params = new URLSearchParams(window.location.search); if (search) params.set('q', search); else params.delete('q'); window.history.replaceState({}, '', `/benchmarks${params.size ? `?${params}` : ''}`) }, [search]); return <section><SearchField value={search} onChange={setSearch} label="Search benchmark catalog" /><CheckboxTree items={tree.filter((item) => item.label.toLowerCase().includes(search.toLowerCase()) || search === '')} selected={selected} onChange={setSelected} /><EmptyState title="Catalog is not connected" detail="The backend catalog will be available in a later stage." /></section> }
function RunPresets() { return <section><Notice>Preset editing is not available until the versioned Run Presets API is integrated.</Notice><EmptyState title="No presets yet" detail="Create presets after the backend contract is available." /></section> }
function Runs() { const [range, setRange] = useState(new URLSearchParams(window.location.search).get('range') ?? '7d'); const [page, setPage] = useState(1); useEffect(() => { const params = new URLSearchParams(window.location.search); params.set('range', range); window.history.replaceState({}, '', `/runs?${params}`) }, [range]); const title = useMemo(() => range === 'all' ? 'No recorded runs' : 'No runs in this range', [range]); return <section><DateRangePicker value={range} onChange={setRange} /><EmptyState title={title} detail="Completed benchmark runs will appear here." /><Pagination page={page} pageCount={1} onPage={setPage} /></section> }

const rootElement = document.getElementById('root')
if (rootElement) createRoot(rootElement).render(<StrictMode><App /></StrictMode>)
export { App }
