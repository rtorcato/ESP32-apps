import { createFileRoute } from "@tanstack/react-router"

// ponytail: placeholder until Zoey's dashboard mockups are signed off.
export const Route = createFileRoute("/dashboard")({ component: Dashboard })

function Dashboard() {
  return <h1 className="font-heading text-3xl font-bold">Dashboard</h1>
}
