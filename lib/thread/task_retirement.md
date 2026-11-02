# Финализация TaskSystem

TaskSystem уничтожает собственные callable captures вне registry, task и ready-queue locks.
Это распространяется на normal/failed completion, прямую и зависимую отмену, shutdown,
immediate-cancelled Submit и отклонённую подготовку/rollback Submit.

Внутренний флаг finalization запрещает повторный запуск и повторную отмену, пока payload
уничтожается. GetStatus сохраняет прежний нетерминальный статус; GetError публикуется вместе
с terminal outcome. Wait и WaitIdle не обгоняют это уничтожение. Dependent work запускается
после уничтожения captures успешного prerequisite. Handle не удерживает callable после terminal.
Внешние копии callable/captures остаются ответственностью пользователя.

Отмена нового dependent допускается уже при FINALIZING неуспешного prerequisite. Поэтому
Wait отменённого потомка не означает, что captures предков уже уничтожены. Чтобы дождаться
всего дерева, следует явно ожидать нужные handles или WaitIdle при закрытом admission.

Destructor capture может вызвать Submit, GetStatus или Cancel другой задачи. При shutdown
Submit отклоняется: destructor обязан перехватить исключение. Cancel финализируемой задачи
идемпотентен; завершение чужой текущей финализации он не ждёт. Destructors должны быть noexcept
и завершаться. Self-wait, WaitIdle из собственной финализации и уничтожение pool из callback
не поддерживаются. Cancel действительно RUNNING задачи по-прежнему выдаёт INVALID_STATE.

Каждый cancellation batch удерживает State собственными pins до публикации terminal и освобождения
outstanding contribution. Обход batch итеративен, State locks не вкладываются. Представление
edges и их линейный поиск остаются прежними: O(V+E) unlink и масштабные DAG-гарантии относятся
к FIX-0002. Полный no-allocation transaction/ready-queue protocol и fault injection относятся
к FIX-0004; этот этап не объявляет их выполненными.
