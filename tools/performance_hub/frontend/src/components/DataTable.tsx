import { useId, useState, type Key, type ReactNode } from 'react'
import { Pagination } from './controls'

export type TableColumn<Row> = { id: string; header: string; cell(row: Row): ReactNode }

export function DataTable<Row>({ caption, rows, columns, rowKey, pageSize = 20, empty = 'No records' }: {
  caption: string
  rows: readonly Row[]
  columns: readonly TableColumn<Row>[]
  rowKey(row: Row): Key
  pageSize?: number
  empty?: string
}) {
  const id = useId()
  const [requestedPage, setPage] = useState(1)
  const size = Number.isSafeInteger(pageSize) && pageSize > 0 ? pageSize : 20
  const pageCount = Math.max(1, Math.ceil(rows.length / size))
  const page = Math.min(requestedPage, pageCount)
  return <section aria-labelledby={id}>
    <div className="table-scroll" role="region" aria-label={caption} tabIndex={0}>
      <table><caption id={id}>{caption}</caption>
        <thead><tr>{columns.map((column) => <th scope="col" key={column.id}>{column.header}</th>)}</tr></thead>
        <tbody>{rows.length === 0
          ? <tr><td colSpan={Math.max(1, columns.length)}>{empty}</td></tr>
          : rows.slice((page - 1) * size, page * size).map((row) => <tr key={rowKey(row)}>{columns.map((column) => <td key={column.id}>{column.cell(row)}</td>)}</tr>)}</tbody>
      </table>
    </div>
    <Pagination page={page} pageCount={pageCount} onPage={setPage} />
  </section>
}
