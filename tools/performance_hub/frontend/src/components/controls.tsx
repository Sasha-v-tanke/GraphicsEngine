import { useId, useMemo } from 'react'

export function SearchField({ value, onChange, label = 'Search' }: { value: string; onChange(value: string): void; label?: string }) {
  const id = useId()
  return <label htmlFor={id} className="control"><span>{label}</span><input id={id} type="search" value={value} onChange={(event) => onChange(event.target.value)} /></label>
}

export function DateRangePicker({ value, onChange }: { value: string; onChange(value: string): void }) {
  const id = useId()
  return <label htmlFor={id} className="control"><span>Date range</span><select id={id} value={value} onChange={(event) => onChange(event.target.value)}><option value="7d">Last 7 days</option><option value="30d">Last 30 days</option><option value="all">All time</option></select></label>
}

export function Pagination({ page, pageCount, onPage }: { page: number; pageCount: number; onPage(page: number): void }) {
  return <nav aria-label="Pagination" className="pagination"><button type="button" disabled={page === 1} onClick={() => onPage(page - 1)}>Previous</button><span>Page {page} of {pageCount}</span><button type="button" disabled={page === pageCount} onClick={() => onPage(page + 1)}>Next</button></nav>
}

export type TreeItem = { id: string; label: string; children?: TreeItem[] }

export function CheckboxTree({ items, selected, onChange }: { items: TreeItem[]; selected: Set<string>; onChange(selected: Set<string>): void }) {
  const allLeaves = useMemo(() => items.flatMap(leaves), [items])
  const allSelected = allLeaves.length > 0 && allLeaves.every((item) => selected.has(item.id))
  const toggleAll = () => onChange(allSelected ? new Set() : new Set(allLeaves.map((item) => item.id)))
  return <section aria-label="Selectable tree" className="tree"><button type="button" disabled={allLeaves.length === 0} onClick={toggleAll}>{allSelected ? 'Select none' : 'Select all'}</button><ul>{items.map((item) => <TreeNode item={item} key={item.id} selected={selected} onChange={onChange} />)}</ul></section>
}

function leaves(item: TreeItem): TreeItem[] { return item.children ? item.children.flatMap(leaves) : [item] }

function TreeNode({ item, selected, onChange }: { item: TreeItem; selected: Set<string>; onChange(selected: Set<string>): void }) {
  if (item.children) return <li><span>{item.label}</span><ul>{item.children.map((child) => <TreeNode key={child.id} item={child} selected={selected} onChange={onChange} />)}</ul></li>
  return <li><label><input type="checkbox" checked={selected.has(item.id)} onChange={() => { const next = new Set(selected); if (next.has(item.id)) next.delete(item.id); else next.add(item.id); onChange(next) }} />{item.label}</label></li>
}
