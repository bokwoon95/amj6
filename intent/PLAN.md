# AutomateMyJob Plan

AutomateMyJob is a C++/Qt desktop automation application and daemon that combines:

- A coding agent.
- A browser/desktop automation runtime.
- A control center for running, scheduling, inspecting, and editing automation scripts.

Unlike OpenClaw, which is a control center for _AI workflows_, AutomateMyJob is primarily a control center for _scripts_. The user creates bash/powershell/python scripts which the AutomateMyJob program can either trigger 1. on demand, 2. on a schedule or 3. in response to an event. During normal script operations, the AI agent is never involved. But the AI agent is the beating heart of AutomateMyJob because:

- The agent can write scripts for the user, so the user never needs to know how to code.
- The agent has access to persistent bash/powershell/python sessions so it can call the same functions that scripts call, using its feedback loop and exploratory phase to determine what function calls are needed before formalizing those same function calls into a reusable script.
- The application exposes GUI extensions via QtQuick so users can ask the agent to create new user interfaces in AutomateMyJob using QML/JavaScript.

There are six builtin tools:

- read
- write
- edit
- bash
- powershell
- python

bash/powershell/python tools are normally stateless, but can also spin up _persistent_ bash/powershell/python sessions where subsequent tool calls  rely on built-up state from previous tool calls to the same session_id.

## Sandboxing

We will eventually need to integrate native sandboxing (based on modes) to prevent agents from accessing external directories and doing disastrous deletes. Since we will be able to intercept all text coming in and out of a mode, the hope is that we can parse (to some degree) what each command issued to a mode is doing and block user-blacklisted command types.

## Database persistence

Agents and scripts may read and write to one or more SQLite databases. This means tables and columns and indexes may also need to be dynamically created. Agents can create tables/columns/indexes on an ad hoc basis just fine, but scripts will use an automigration SQL library (defined in amj/ core) for declaratively requiring a certain database schema and it will automatically be created if not exists by the library.

Agents and scripts may also connect directly to Postgres or MySQL databases hosted elsewhere, but that's between them and the database host. Database credentials will be stored in a similar fashion to how DBeaver does it.

## Events

AutomateMyJob provides a global event bus in the runtime. Scripts can be run either on a schedule or in response to events from the event bus. Scripts themselves can also publish events to the event bus, allowing a persistent polling script to convert polling to notification-style events that wake up other scripts.
