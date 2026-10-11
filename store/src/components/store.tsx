import { Link } from "@tanstack/react-router"
import type { ReactNode } from "react"
import { BoardFrame } from "@/components/board-frame"
import { buttonVariants } from "@/components/ui/button"
import type { App } from "@/data/apps"
import type { Board } from "@/data/boards"
import { AMAZON_DISCLOSURE, amazonUrl } from "@/lib/amazon"
import { cn } from "@/lib/utils"

export function Shell({ children }: { children: ReactNode }) {
  return (
    <div className="flex min-h-svh flex-col">
      <header className="border-b bg-card">
        <nav className="mx-auto flex h-16 max-w-7xl items-center justify-between gap-6 px-4 md:h-[72px] md:px-8">
          <Link
            to="/"
            className="flex items-center gap-2.5 font-heading text-[22px] font-bold tracking-tight"
          >
            <span
              aria-hidden
              className="grid size-7 place-items-center rounded-[7px] bg-primary text-[15px] text-white"
            >
              ⚡
            </span>
            Flashcart
          </Link>
          <div className="flex gap-5 text-[15px] font-medium text-muted-foreground md:gap-7">
            {(
              [
                ["/apps", "Apps"],
                ["/", "Boards"],
                ["/dashboard", "Dashboard"],
              ] as const
            ).map(([to, label]) => (
              <Link
                key={to}
                to={to}
                activeOptions={{ exact: to === "/" }}
                activeProps={{ className: "text-foreground" }}
                className="hover:text-foreground"
              >
                {label}
              </Link>
            ))}
          </div>
        </nav>
      </header>
      <main className="mx-auto w-full max-w-7xl flex-1 px-4 py-8 md:px-8 md:py-12">
        {children}
      </main>
      <footer className="border-t bg-card">
        <div className="mx-auto flex max-w-7xl flex-col gap-1 px-4 py-6 text-[13px] text-muted-foreground md:px-8">
          <p>We don’t sell hardware. {AMAZON_DISCLOSURE}</p>
          <p className="font-mono text-xs">
            ESP32 is a trademark of Espressif Systems. Flashcart is not
            affiliated with Espressif.
          </p>
        </div>
      </footer>
    </div>
  )
}

export function Panel({
  className,
  children,
}: {
  className?: string
  children: ReactNode
}) {
  return (
    <section className={cn("rounded-2xl border bg-card", className)}>
      {children}
    </section>
  )
}

export function Price({
  price,
  className,
}: {
  price: string
  className?: string
}) {
  return (
    <span
      className={cn(
        "font-mono text-xs font-semibold",
        price === "FREE" && "text-free",
        className
      )}
    >
      {price}
    </span>
  )
}

/** External Amazon link: sponsored, new tab, and says so to screen readers. */
export function AmazonLink({
  href,
  className,
  children,
}: {
  href: string
  className?: string
  children: ReactNode
}) {
  return (
    <a
      href={amazonUrl(href)}
      target="_blank"
      rel="sponsored noopener"
      className={className}
    >
      {children}
      <span className="sr-only"> (opens Amazon)</span>
    </a>
  )
}

export function Disclosure({ className }: { className?: string }) {
  return (
    <p className={cn("text-[13px] text-muted-foreground", className)}>
      {AMAZON_DISCLOSURE}
    </p>
  )
}

export function BuyBox({ board }: { board: Board }) {
  return (
    <div className="rounded-xl border bg-[#F8FAFC] p-5">
      <div className="flex flex-col gap-2.5 sm:flex-row">
        <AmazonLink
          href={board.amazon.ca}
          className={cn(
            buttonVariants({ variant: "ink", size: "xl" }),
            "flex-1"
          )}
        >
          Check price on Amazon.ca ↗
        </AmazonLink>
        <AmazonLink
          href={board.amazon.com}
          className={cn(
            buttonVariants({ variant: "outline", size: "xl" }),
            "flex-1 bg-card"
          )}
        >
          Amazon.com ↗
        </AmazonLink>
      </div>
      <p className="mt-3 text-[13px] text-muted-foreground">
        We don’t sell hardware. {AMAZON_DISCLOSURE}
      </p>
    </div>
  )
}

/** Without a board, shows the app on the first board it runs on. */
export function AppTile({ app, board }: { app: App; board?: Board }) {
  return (
    <Link
      to="/apps/$app"
      params={{ app: app.slug }}
      search={board ? { board: board.slug } : {}}
      className="group overflow-hidden rounded-xl border bg-card transition-colors hover:border-primary"
    >
      <div className="grid h-[150px] place-items-center bg-[#0B0F17]">
        <img
          src={
            board
              ? app.screens.get(board.slug)
              : app.screens.values().next().value
          }
          alt=""
          className="h-[132px] max-w-[90%] object-contain"
        />
      </div>
      <div className="px-3 py-2.5">
        <b className="block font-heading text-sm font-semibold">{app.name}</b>
        <Price price={app.price} />
      </div>
    </Link>
  )
}

// Zoey's card scales: c6 3.2, cyd 2.0, s3 1.0, r21 2.3.
const CARD_SCALE: Record<string, number> = {
  "esp32-c6-lcd-1.47": 3.2,
  "esp32-2432s028r-cyd": 2.0,
  "esp32-s3-touch-lcd-7": 1.0,
  "elecrow-rotary-2.1": 2.3,
  "elecrow-rotary-1.28": 3.2,
}

export function BoardCard({
  board,
  appCount,
  heroSrc,
}: {
  board: Board
  appCount: number
  heroSrc: string | null
}) {
  return (
    <article className="relative overflow-hidden rounded-xl border bg-card">
      <div className="grid h-[190px] place-items-center stage">
        <BoardFrame
          board={board}
          src={heroSrc}
          scale={CARD_SCALE[board.slug]}
        />
      </div>
      <div className="p-3.5">
        <h3 className="font-heading text-base font-semibold">
          <Link
            to="/boards/$board"
            params={{ board: board.slug }}
            className="after:absolute after:inset-0 after:content-[''] focus-visible:outline-none focus-visible:after:rounded-xl focus-visible:after:outline-2 focus-visible:after:outline-offset-2 focus-visible:after:outline-ring"
          >
            {board.maker} {board.short}
          </Link>
        </h3>
        <p className="mt-0.5 mb-2.5 font-mono text-xs text-muted-foreground">
          {board.meta}
        </p>
        <div className="flex items-center justify-between text-[13px]">
          <span>{appCount} apps</span>
          {/* relative z-10 lifts the Amazon link above the card-wide link. */}
          <AmazonLink
            href={board.amazon.ca}
            className="relative z-10 font-semibold text-primary hover:underline"
          >
            Amazon ↗
          </AmazonLink>
        </div>
      </div>
    </article>
  )
}
