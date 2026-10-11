import { Link, createFileRoute, notFound } from "@tanstack/react-router"
import { useState } from "react"
import { BoardFrame } from "@/components/board-frame"
import { AppTile, BuyBox, Panel } from "@/components/store"
import { appsForBoard } from "@/data/apps"
import { boardBySlug } from "@/data/boards"
import { cn } from "@/lib/utils"

export const Route = createFileRoute("/boards/$board")({
  loader: ({ params }) => {
    const board = boardBySlug(params.board)
    if (!board) throw notFound()
    return { board }
  },
  head: ({ loaderData }) => ({
    meta: [{ title: `${loaderData?.board.name} · Flashcart` }],
  }),
  component: BoardPage,
})

const BOX_ICON: Record<string, string> = {
  "Board with screen": "▭",
  "USB cable": "⌁",
  "Touch stylus": "✎",
}

function BoardPage() {
  const { board } = Route.useLoaderData()
  const apps = appsForBoard(board.slug)
  // Keyed by slug so the pick falls back cleanly when navigating to another board.
  const [pick, setPick] = useState(board.heroApp)
  const shown =
    apps.find((a) => a.slug === pick) ??
    apps.find((a) => a.slug === board.heroApp) ??
    apps.at(0)
  const size =
    "d" in board.frame
      ? `Ø ${board.frame.d} mm`
      : `${board.frame.h} × ${board.frame.w} mm`
  // Hero at 4.6 px/mm (Zoey), shrunk to fit the stage for the 7″.
  const scale = Math.min(
    4.6,
    480 / ("d" in board.frame ? board.frame.d : board.frame.w)
  )

  return (
    <Panel>
      <div className="grid gap-8 p-4 md:p-10 lg:grid-cols-[minmax(0,560px)_1fr] lg:gap-12">
        <div>
          <div className="relative grid h-[380px] place-items-center overflow-hidden rounded-xl stage md:h-[520px]">
            <div className="absolute top-3.5 left-3.5 flex rounded-full border bg-card p-[3px] font-mono text-xs">
              <span className="rounded-full bg-foreground px-3 py-1 text-white">
                Running an app
              </span>
              {/* Real photos come later; until then these stay disabled. */}
              <button
                type="button"
                disabled
                className="px-3 py-1 text-muted-foreground"
              >
                Photo
              </button>
              <button
                type="button"
                disabled
                className="px-3 py-1 text-muted-foreground"
              >
                Back
              </button>
            </div>
            <BoardFrame
              board={board}
              src={shown?.screens.get(board.slug) ?? null}
              scale={scale}
              alt={shown ? `${shown.name} running on ${board.short}` : ""}
            />
            <p className="absolute right-4 bottom-3.5 font-mono text-xs text-muted-foreground">
              ≈ {size}
            </p>
          </div>
          {apps.length > 1 && (
            <div
              className="mt-3.5 flex gap-2.5 overflow-x-auto pb-1"
              role="group"
              aria-label="Show another app"
            >
              {apps.map((a) => (
                <button
                  key={a.slug}
                  type="button"
                  aria-pressed={a === shown}
                  aria-label={a.name}
                  onClick={() => setPick(a.slug)}
                  className={cn(
                    "grid size-[76px] flex-none place-items-center overflow-hidden rounded-[10px] border bg-[#F7F9FB]",
                    a === shown && "border-2 border-primary"
                  )}
                >
                  <img
                    src={a.screens.get(board.slug)}
                    alt=""
                    className="max-h-[62px] max-w-[62px] object-contain"
                  />
                </button>
              ))}
            </div>
          )}
        </div>

        <div>
          <p className="font-mono text-[13px] text-muted-foreground">
            <Link to="/" className="hover:text-foreground">
              Boards
            </Link>{" "}
            / {board.short}
          </p>
          <h1 className="mt-2.5 mb-1.5 font-heading text-3xl leading-[1.1] font-bold tracking-[-0.025em] md:text-[40px]">
            {board.name}
          </h1>
          <p className="text-muted-foreground">{board.tagline}</p>
          <ul className="my-5 flex flex-wrap gap-2">
            {board.chips.map((c) => (
              <li
                key={c}
                className="inline-flex h-8 items-center rounded-full bg-muted px-3 font-mono text-[13px] font-medium"
              >
                {c}
              </li>
            ))}
          </ul>
          <BuyBox board={board} />

          <div className="mt-7 grid gap-7 sm:grid-cols-2">
            <div>
              <h2 className="mb-2.5 font-heading text-[17px] font-semibold">
                Specs
              </h2>
              <table className="w-full text-sm">
                <tbody>
                  {board.specs.map(([k, v]) => (
                    <tr key={k} className="border-b">
                      <th
                        scope="row"
                        className="w-[42%] py-[7px] text-left font-mono text-[13px] font-normal text-muted-foreground"
                      >
                        {k}
                      </th>
                      <td className="py-[7px]">{v}</td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
            <div>
              <h2 className="mb-2.5 font-heading text-[17px] font-semibold">
                In the box
              </h2>
              <ul className="flex flex-col gap-2 text-sm">
                {board.inBox.map((item) => (
                  <li key={item} className="flex items-center gap-2.5">
                    <i
                      aria-hidden
                      className="grid size-[22px] place-items-center rounded-md bg-muted text-xs not-italic"
                    >
                      {BOX_ICON[item] ?? "⋯"}
                    </i>
                    {item}
                  </li>
                ))}
              </ul>
              <p className="mt-5 rounded-r-[10px] border-l-[3px] border-primary bg-accent px-4 py-3.5 text-sm">
                <b>Pick this if</b> {board.pickIf}
              </p>
            </div>
          </div>
        </div>
      </div>

      <div className="px-4 pb-4 md:px-10 md:pb-10">
        <h2 className="mb-2.5 font-heading text-[17px] font-semibold">
          {apps.length} {apps.length === 1 ? "app runs" : "apps run"} on this
          board
        </h2>
        <div className="grid grid-cols-2 gap-3.5 sm:grid-cols-3 lg:grid-cols-6">
          {apps.map((a) => (
            <AppTile key={a.slug} app={a} board={board} />
          ))}
        </div>
      </div>
    </Panel>
  )
}
