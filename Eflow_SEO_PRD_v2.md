# Eflow SEO Growth & Developer Page PRD

**Product:** Eflow website  
**Document type:** Product Requirements Document + execution contract for Claude Code  
**Status:** Ready for repository audit  
**Primary rule:** Audit first. Report to the owner. Stop. Implement only after explicit approval.  
**Canonical domain / stack / audience:** Must be discovered from the actual project and verified; do not assume these from this PRD.

---

## 1. Executive mandate

Build the strongest sustainable organic-search foundation possible for Eflow, then improve its ability to attract relevant non-brand search traffic and convert that traffic into meaningful outcomes. Give the **Developer page** a separate, high-priority SEO workstream and ensure it receives a distinct strategy, evidence-based recommendations, implementation, and validation.

The project must not begin by blindly rewriting metadata, adding generic blog posts, or installing SEO libraries. Claude must first inspect the existing repository and public site, determine what is already in place, and return an evidence-based audit. No implementation is authorized until the owner approves the audit and proposed plan.

The objective is to maximize Eflow's legitimate eligibility, relevance, quality, discoverability, and competitiveness in search. No person or tool can guarantee a first-place ranking, instant indexing, or a particular traffic increase. Prioritize the highest-impact verified blockers and opportunities; do not imitate spam tactics or chase vanity scores.

## 2. Context and assumptions

The exact Eflow domain, business model, target audience, framework, current route structure, and meaning of “Developer page” are not specified in this document. Claude must discover them from the current repository, existing project documentation, deployment configuration, and the production site where accessible.

Do not reuse SafeShare’s product descriptions, claims, feature assumptions, keyword ideas, metadata, or content. This PRD applies exclusively to Eflow.

“Developer page” may refer to a page about a specific developer, a developer-facing product or documentation page, a developer profile/portfolio, or another existing route. Inspect the actual page before choosing a strategy. If the evidence does not resolve what this page is meant to accomplish, report that uncertainty and request clarification in the audit report; do not make an irreversible assumption.

## 3. Goals and non-goals

### 3.1 Goals

1. Make all intended public Eflow pages technically eligible to be crawled and considered for indexing.
2. Identify and remove verified technical, rendering, duplication, metadata, internal-link, and performance barriers.
3. Align every important indexable page with a distinct, researched user need and search intent.
4. Develop an evidence-based keyword and information architecture plan that fits Eflow’s actual purpose and capabilities.
5. Treat the Developer page as a priority landing page with its own user intent, competitive landscape, on-page plan, credibility requirements, and measurement.
6. Build helpful, original content and real authority signals instead of thin pages, keyword stuffing, or mass-produced articles.
7. Establish measurement through Google Search Console and analytics where access and consent are available.
8. Deliver tested changes, a traceable summary of files changed, known limitations, and a prioritized next-step backlog.

### 3.2 Non-goals

- Promise a number-one ranking, a guaranteed traffic multiplier, or instant indexing.
- Purchase links, use private blog networks, automate link spam, cloak content, stuff keywords, or publish doorway pages.
- Generate large volumes of low-value AI text simply to occupy search results.
- Invent search volume, keyword difficulty, competitor metrics, user testimonials, credentials, project outcomes, product capabilities, or structured-data facts.
- Make unrelated visual/product changes or refactor working architecture without a demonstrated SEO or user-experience reason.
- Submit forms, publish releases, contact third parties, change external platform settings, or deploy to production without explicit authorization.
- Expose credentials, private configuration, user data, internal URLs, unpublished work, or confidential information.

## 4. Mandatory Claude execution protocol

This protocol overrides any temptation to jump straight into implementation.

### Phase 0 — Establish the project safely

1. Inspect the repository root, project instructions, README, route structure, package manifest, framework, build/test scripts, deployment configuration, and existing SEO-related files/components.
2. Identify the production domain from trustworthy project/deployment configuration. Verify it against the live site if internet access is available. If multiple domains or environments conflict, report the ambiguity instead of choosing silently.
3. Inspect the current Git status and existing local changes before doing anything else. Never discard, reset, overwrite, or reformat pre-existing work.
4. Identify the actual Developer page route and classify its purpose from the page content, navigation, product documentation, and repository evidence.
5. Use read-only commands and non-invasive checks for this phase. Do not alter source files, create or modify project files, install packages, run automatic formatters, update dependencies, modify the database, edit external account settings, or deploy anything.
6. If live checks, Search Console data, analytics, keyword tools, or credentials are unavailable, mark the corresponding findings **Unverified** or **Blocked** and explain exactly what evidence is missing. Never claim to have checked a system that was inaccessible.

### Phase 1 — Deliver the audit and STOP

Return a report to the owner before implementation. The report must include:

- A concise executive summary and the most consequential findings.
- A page/route inventory, with public, private, utility, duplicate, and uncertain routes classified separately.
- A requirements matrix using only these statuses: **Implemented**, **Partial**, **Missing**, **Not applicable**, or **Unverified / Blocked**.
- Evidence for every status: relevant file path and line/component when available, route/URL and observed behavior, command/test output, or an explicit explanation of why evidence is unavailable.
- Separate findings for the Developer page, including its purpose, route, current indexability, content, metadata, internal links, structured data, credibility signals, performance, and query/intent opportunities.
- A prioritized remediation backlog with severity, impact, effort, dependency, risk, and a clear reason for priority.
- A proposed implementation plan and acceptance tests.
- The changes that are intentionally out of scope.
- Any decisions or external access needed from the owner.

**Hard stop:** After presenting the audit, do not implement, edit, install, publish, deploy, submit URLs, or make external changes. Wait for explicit owner approval. A request to “audit,” this PRD, or a general desire for SEO does not constitute approval to change the codebase. Accept approval only when the owner explicitly says to proceed with implementation (or approves a clearly defined subset).

### Phase 2 — Implement only approved work

After approval:

1. Work from the approved backlog, starting with verified critical blockers and high-impact fixes.
2. Implement in small, logically grouped changes. Preserve existing design, functionality, accessibility, security, and code conventions.
3. Reuse existing framework capabilities where practical. Do not add dependencies unless their benefit is clear and approved by necessity; prefer a small native implementation over an unnecessary SEO package.
4. Do not change the production domain, hosting, analytics ownership, DNS, robots behavior, indexing policy, or external services without explicit permission.
5. Before making any page indexable, confirm that it is intended for the public search index and contains no private, sensitive, duplicate, placeholder, or low-value material.
6. Run the project’s relevant lint, type-check, build, unit, integration, and end-to-end checks that are available. Add focused tests when the repository supports them.
7. Inspect rendered HTML and browser output for important pages, including the Developer page. Validate metadata, canonical tags, links, robots directives, sitemap entries, schema, and redirect/status behavior after the changes.
8. Report what changed, files affected, tests run and outcomes, unresolved issues, risks, and any manual steps that still require owner access.
9. Do not deploy to production or perform external submissions unless the owner explicitly approved those actions.

## 5. Audit status and evidence standard

| Status | Meaning |
|---|---|
| Implemented | The requirement exists and evidence confirms it works as intended. |
| Partial | Some elements exist, but important gaps or defects remain. |
| Missing | Evidence confirms the requirement is absent or broken. |
| Not applicable | The requirement does not fit the product/site, with a brief reason. |
| Unverified / Blocked | Evidence is insufficient, tools/access are unavailable, or the result cannot be confirmed safely. |

Do not label an item “Missing” simply because a tool could not check it. Do not label it “Implemented” simply because a relevant file exists. Verify behavior whenever possible.

Each audit finding should have these fields: ID, requirement, status, evidence, affected routes, user/search impact, risk/severity, proposed remediation, estimated effort (S/M/L), dependencies, and validation method.

## 6. Product outcomes and measurement

### 6.1 North-star outcome

Increase **qualified organic search traffic** to Eflow and the proportion of that traffic that completes the website’s real intended outcome. Determine the meaningful conversion only after discovering what Eflow does; do not invent a conversion event.

### 6.2 Required measurement

Establish an initial baseline before evaluating improvement. Where data exists, capture at least the previous 28 days and compare with the preceding period; also account for seasonality and site age where relevant. If data is unavailable, document that limitation and define a forward baseline date.

Track, where available:

- Google Search Console organic impressions, clicks, click-through rate, average position, queries, and landing pages.
- Branded versus non-branded query performance, using documented classification rules.
- Indexing and exclusion status of intended public URLs.
- Developer-page impressions, clicks, queries, CTR, average position, referring links, and relevant conversion events.
- Organic sessions/users and the site’s actual primary conversion in analytics, where consent and configuration permit.
- Core Web Vitals field data and lab diagnostics.
- Valid indexed URLs versus intended indexable URLs, canonical selection, crawl errors, and sitemap processing status.
- Content outcomes: qualified query coverage, impressions, clicks, assisted conversions where measurable, and relevant referring domains.

Do not set arbitrary traffic-growth commitments before the baseline, current index status, audience, and competitive conditions are known. After the audit, propose 30-, 60-, and 90-day measurable targets, label estimates as estimates, and state assumptions.

## 7. Comprehensive SEO requirements

The following are acceptance requirements to evaluate, not permission to implement before the approval gate.

### 7.1 Technical crawlability and indexability

For every intended public page, verify:

- It responds correctly (normally HTTP 200 for a canonical live page), loads without fatal errors, and provides crawlable indexable content.
- The page is not unintentionally blocked by `robots.txt`, `noindex`, authentication, or rendering/configuration errors.
- A canonical URL is specified where useful and agrees with the site’s redirect, sitemap, internal-link, and deployment behavior.
- Redirects are intentional, stable, and avoid chains/loops. Nonexistent pages return an appropriate 404/410 behavior rather than silently serving a 200 homepage.
- The site has a consistent HTTPS and hostname policy, supported by correct redirects and canonicals.
- Search engines can reach key pages through ordinary crawlable `<a href="...">` links, not solely through button event handlers or inaccessible client state.
- Important routes work when accessed directly, refreshed, and visited without first navigating from the homepage.
- Query parameters, tracking parameters, pagination, filters, alternate routes, trailing slashes, and case variants do not create avoidable duplicate URL sets.
- Private routes, account pages, internal dashboards, temporary routes, and sensitive user-generated pages are protected appropriately and excluded from public indexing where necessary.

Do not use `robots.txt` as a substitute for access control or as the only mechanism to keep a sensitive page out of search results. Do not add `noindex` broadly without evaluating current visibility and intended public routes.

### 7.2 Sitemap and robots.txt

Inspect whether `robots.txt` and an XML sitemap already exist and are correct before proposing changes.

If needed and approved, build or repair a sitemap that:

- Uses absolute canonical production URLs.
- Includes only intended indexable canonical pages that should appear in search results.
- Excludes redirects, 404s, private pages, duplicate URLs, and `noindex` URLs.
- Is updated automatically when relevant public pages are added or changed, using the framework’s existing routing/data model.
- Is discoverable through `robots.txt` when appropriate and is submit-ready for Search Console.

A sitemap helps discovery; it does not guarantee crawling, indexing, or ranking. Do not submit a sitemap through an external account unless authorized.

### 7.3 Rendering and JavaScript SEO

Identify the framework and current rendering mode first. Check what unauthenticated users and search crawlers receive for each important route. Confirm title, description, primary heading, main content, canonical, robots directives, internal links, and structured data are present in the rendered result and accessible in a representative HTML response.

Use server rendering, static generation, prerendering, or another appropriate rendering pattern when it improves reliable discovery and delivery for public content. Do not perform a costly framework migration solely for SEO without evidence that the current approach blocks indexing or materially harms performance. Ensure every distinct piece of indexable content has a stable, shareable URL.

### 7.4 Metadata and on-page fundamentals

Every important indexable page needs:

- A unique, specific, accurate `<title>` that reflects the actual page and likely user intent.
- A concise, useful meta description where appropriate; do not treat the description as a guaranteed ranking factor or guarantee Google will display it.
- One clear primary page heading and a logical heading hierarchy.
- Meaningful visible main content that addresses the page’s purpose, not a nearly empty template.
- A descriptive, readable URL where feasible without breaking existing URLs.
- A relevant canonical URL and correct robots directives.
- Useful, crawlable internal links with descriptive anchor text.
- Appropriate image dimensions, compression, and descriptive alternative text for meaningful images; decorative images should not receive keyword-stuffed alt text.
- Correct language, encoding, viewport, favicon, social sharing metadata, and Open Graph image where relevant.
- Metadata generated from the source of truth for each route rather than duplicated defaults across every page.

Do not stuff exact-match phrases into every heading, title, image alt, or URL. Keep copy human-readable and accurate. Do not replace all page content with keyword variations.

### 7.5 Keyword research and search intent

After discovering Eflow’s actual offering, audience, geography/language, and monetization or product goal, build an evidence-based keyword map.

Required process:

1. Identify the site’s real audience, use cases, differentiators, vocabulary, and important conversion paths from project evidence and the public site.
2. Research relevant search queries and current search results. Examine the intent and type of pages ranking for the query, competitors’ strengths/weaknesses, likely difficulty, and whether Eflow can genuinely satisfy the query.
3. Use reliable available keyword data when possible. Record the tool/source and date. If volume/difficulty data is unavailable, use qualitative intent-based estimates and clearly label them; never invent numerical search volume, difficulty, or traffic projections.
4. Group related queries by intent and map one primary intent to one best-fit page. Identify cannibalization risks and pages that should be consolidated rather than expanded.
5. Prioritize by relevance to Eflow, opportunity to satisfy the intent, realistic competitiveness, user value, business outcome, implementation cost, and evidence quality.
6. Include low-volume/high-intent queries when they fit the actual site, as well as broader category queries only where the site can credibly compete.
7. Revisit keyword mapping after Search Console produces actual impressions and query data.

Do not create a separate page for every minor keyword variation, city, spelling, or adjective. Do not target a phrase that the page cannot honestly fulfill.

### 7.6 Information architecture and internal linking

Build a clear path from the home page/navigation to all high-value public pages. Important pages should not be orphaned. Use contextual links between complementary pages, guides, case studies/projects, the Developer page, and relevant product/service pages.

Audit navigation labels, breadcrumbs where useful, footer links, page hierarchy, related-content links, and anchor text. Links should help users first and make the target page understandable. Avoid excessive sitewide exact-match anchors or unnecessary duplicate routes.

If the site has programmatically generated routes, review crawl quality, pagination, filters, and duplicate-content risks before expanding them.

### 7.7 Content quality and editorial strategy

Build content for a real audience, with demonstrable usefulness and original value. Content may include detailed guides, comparisons, how-to resources, case studies, project write-ups, documentation, templates/tools, original research, or other formats justified by Eflow’s actual purpose.

Every proposed piece must specify: target audience, user problem, search intent, evidence/source of demand, unique value Eflow can add, outline, authoritative sources if facts are involved, internal links, desired conversion, and a post-publication measurement plan.

Content must be fact-checked and reviewed. If using AI to assist drafting, the output must be edited for accuracy, originality, completeness, and usefulness. Do not mass-publish generic articles, lightly paraphrase competitors, fabricate personal experience, falsify dates, invent reviews, or add content solely to manufacture keyword coverage.

Do not impose a word count without a user-value rationale. A smaller page that fully answers the intent is preferable to a padded article.

### 7.8 Authority, reputation, and backlinks

Create a realistic, evidence-based authority plan after the audit. Prefer earning mentions and links through useful projects, original research, open-source work, community participation, legitimate partnerships, quality documentation, expert commentary, and genuinely useful resources.

Never buy links to manipulate rankings, use private blog networks, automate comments/forum posts, create fake profiles, run mass guest-post schemes, exchange links at scale, or use other tactics that violate search spam policies. Avoid outreach that misrepresents Eflow or its credentials. Track high-relevance referring domains and link quality instead of treating raw backlink count as the goal.

### 7.9 Structured data

Audit existing schema before adding or changing it. Use JSON-LD when appropriate and practical. Select types based on real page content and Google’s current documented eligibility rules; validate the markup and rendered page.

Potential types to consider only when factually applicable include `Organization`, `WebSite`, `BreadcrumbList`, `Article`/`BlogPosting`, `Person`, `ProfilePage`, `SoftwareApplication`, `Product`, or other documented types supported by the content. Do not add irrelevant schema merely to make the page look sophisticated.

Every property must be accurate and visible/consistent with the page. Do not fabricate reviews, aggregate ratings, FAQs, pricing, availability, qualifications, social accounts, or product features. Explain that valid markup may make a page eligible for supported search features but does not guarantee a rich result or higher position.

### 7.10 Performance and page experience

Measure performance rather than guessing. Test the homepage, the Developer page, and key public landing pages on mobile and desktop, using both lab diagnostics and field data where available.

Core Web Vitals good thresholds are:

- Largest Contentful Paint (LCP): 2.5 seconds or less.
- Interaction to Next Paint (INP): 200 milliseconds or less.
- Cumulative Layout Shift (CLS): 0.1 or less.

Target these at the 75th percentile when field data is available. Use relevant optimizations such as image sizing/compression, modern image formats, caching, reducing unnecessary JavaScript, code splitting, avoiding render-blocking work, reserving layout space, and fixing long tasks. Choose optimizations based on measured bottlenecks and retain the site’s intended visual identity and functionality. Do not optimize a single synthetic score at the expense of real visitors.

### 7.11 Accessibility and mobile usability

Ensure semantic HTML, accessible names, keyboard navigation, readable content, adequate contrast, clear focus states, responsive layouts, properly labeled forms, useful alternative text, and no critical content hidden on mobile. These are user-quality requirements and can also improve content comprehension and interaction. Do not treat accessibility as a keyword hack.

### 7.12 International and local search (only if relevant)

Discover whether Eflow serves multiple languages, countries, or local areas. If so, inspect language-specific URLs, visible translations, locale signals, and `hreflang` where appropriate; ensure equivalents actually exist. Do not create fake geographic pages or claim local offices/availability without evidence. If the site is global and monolingual, do not add irrelevant local SEO configuration.

### 7.13 Analytics and Search Console

Inspect current analytics and Search Console integration through source/configuration where possible, without printing or exposing secrets. Distinguish source-code setup from confirmed live data collection.

If configuration is missing, explain what is required and propose the least invasive implementation. Never create accounts, accept terms, change property ownership, connect personal accounts, or expose tracking credentials without explicit authorization. Track meaningful conversions without collecting unnecessary sensitive data.

## 8. Dedicated Developer page workstream — high priority

The Developer page must receive special treatment, not just a title-tag edit. Discover its real purpose first and select the applicable branch below.

### 8.1 If it is a personal developer profile / portfolio page

Audit and improve the page around legitimate discovery for the actual person and their relevant professional identity. Verify claims from the existing site/repository; never invent degrees, employer names, awards, clients, years of experience, user numbers, or project results.

Requirements to evaluate:

- A clear, unique page title and meta description that identify the person/role/topic accurately and distinguish the page from generic portfolio pages.
- A strong, visible primary heading and introductory summary that explain the developer’s real specialties and the type of work shown.
- Substantial, original project case studies: problem, role, actual technology, constraints, implementation, trade-offs, and verifiable outcomes where available.
- Relevant skills and technologies supported by demonstrable projects or experience, not an inflated list added for keywords.
- Appropriate links to authentic profiles, public repositories, live projects, technical writing, talks, or other proof, where these exist.
- A professional contact/conversion path that works and does not reveal private contact data unnecessarily.
- Search-intent mapping based on the actual name, specialty, portfolio intent, project topics, and target audience. Separate branded/name queries from generic developer-service queries and avoid trying to rank for impossibly broad terms without a credible plan.
- Appropriate image alt text and an optimized portrait/project imagery treatment where images contribute information.
- `ProfilePage` / `Person` structured data only when the page’s primary focus is genuinely that individual and the properties accurately match visible content. Use `sameAs` only for verified profiles representing the same person.
- Internal links from relevant site pages and contextual links to case studies/projects, ensuring the profile is not orphaned.
- Credibility, consistency, authorship, and clear dates on substantial work where relevant. Do not fabricate testimonials, client logos, certifications, social proof, or outcomes.

### 8.2 If it is a developer-facing product, API, or documentation page

Optimize it for the specific developer job-to-be-done instead of treating it as a personal profile. Audit:

- A specific title/heading tied to the actual API, SDK, integration, or developer task.
- Crawlable and well-structured documentation with stable URLs and clear navigation.
- Accurate quick-start instructions, examples, prerequisites, API references, errors, authentication explanations, versioning, and troubleshooting when applicable.
- Code examples that work and do not expose secrets or encourage insecure implementation.
- Internal links between guides, reference docs, examples, changelog, and the relevant product pages.
- Search-friendly rendering, canonicalization, version/language handling, and documentation sitemap inclusion as appropriate.
- `TechArticle`, `SoftwareApplication`, `BreadcrumbList`, or other structured data only when supported by the actual content and Google’s documented rules.
- A distinction between public docs that should be indexed and private/internal docs that must not be exposed.

### 8.3 If it is another kind of Developer page

Document the page’s actual purpose, audience, and desired outcome. Build an appropriate page-specific SEO plan from evidence. Do not force the personal-profile or documentation checklist onto a page where it does not apply.

### 8.4 Developer page acceptance gate

The audit is incomplete unless the report gives the Developer page its own section with: verified URL and page type; current title, description, H1 and index status; rendering and canonical/robots status; content and intent assessment; current internal/external linking; structured-data assessment; performance evidence; candidate query groups with evidence/uncertainty; competition notes; prioritized issues; and a clear proposed target state.

Following approved implementation, validate this page independently and report the specific changes, tests, and evidence.

## 9. Content/page opportunity plan

Do not pre-commit to a number of landing pages or articles. First discover Eflow’s true offer and existing content, then recommend a page map. Candidate page types might include a clear primary landing page, relevant feature/use-case pages, trust/about/contact pages, product or service detail pages, project/case-study pages, developer/profile or documentation pages, and useful educational guides. Only create a page if there is a distinct audience need and enough original substance to satisfy it.

For every proposed page, document:

- Purpose and audience.
- One primary search intent, with related query clusters.
- Why the page deserves to exist separately from current pages.
- Unique value/proof Eflow can provide.
- Recommended URL and internal links.
- Proposed title and primary heading (clearly marked as drafts).
- Content outline and factual sources needed.
- Conversion goal and success metric.
- Cannibalization/duplication risks.

Use existing pages where they already satisfy the intent; improve or consolidate rather than creating near-duplicates.

## 10. Prioritization framework

Classify findings using both severity and confidence.

- **P0 — Critical blocker:** Public pages are unavailable or unintentionally blocked, serious indexing misconfiguration, security/privacy exposure, or another issue that directly prevents intended discoverability or puts users/data at risk.
- **P1 — High impact:** Important public pages are missing or wrong in core metadata, rendering, canonical/indexability, navigation, or substantial content; Developer page has a clear high-impact defect; or a measured performance issue materially affects key journeys.
- **P2 — Growth opportunity:** Evidence-backed keyword targeting, internal-link improvements, content quality, structured data, or relevant credibility/backlink opportunities.
- **P3 — Refinement:** Lower-confidence or lower-impact polish that should not displace verified blockers.

For each item, include confidence (high/medium/low), evidence, impact hypothesis, cost, dependencies, and the test that will prove completion. Do not confuse estimated SEO impact with a guaranteed outcome.

## 11. Proposed rollout after approval

Timing is a planning framework, not a promise that Google will rank pages within that period.

### Stage A — Crawl/indexation and high-risk issues

Fix approved P0/P1 problems first. Validate routes, response codes, canonicalization, robots directives, crawlable links, rendered content, and sitemap correctness. Protect any sensitive or private routes.

### Stage B — Core pages and Developer page

Improve the homepage and the most important commercial/product/service pages based on actual purpose and intent. Run a dedicated Developer page implementation based on its verified type. Add appropriate internal links and factual structured data if justified.

### Stage C — Performance, analytics, and validation

Fix measured high-impact performance issues. Confirm analytics/Search Console status to the extent access permits. Run all tests and capture a post-change evidence record.

### Stage D — Content and authority experiments

Publish or improve a small number of high-value content assets selected from real query/intent research. Begin legitimate distribution and link earning. Measure early impressions and clicks, then update the plan based on evidence instead of publishing at arbitrary volume.

### Stage E — 30/60/90-day iteration

Use Search Console and analytics data to identify rising queries, high-impression/low-CTR pages, relevant pages that are not indexed, and content that does or does not convert. Prioritize based on observed performance and user needs. Re-check technical health after major releases.

## 12. Acceptance criteria

The project is considered implementation-ready only after the audit report is delivered and the owner approves a defined scope. Approved implementation is complete when all applicable criteria below are met or remaining exceptions are explicitly documented.

### Audit acceptance

- [ ] Correct Eflow repository and canonical production domain identified, or ambiguity explicitly reported.
- [ ] Existing working-tree changes preserved and noted.
- [ ] Actual routes inventoried and categorized.
- [ ] Requirements matrix covers technical, on-page, content, performance, measurement, authority, structured data, privacy, and Developer page areas.
- [ ] Each finding has verifiable evidence or is honestly marked unverified/blocked.
- [ ] Developer page has its own complete analysis.
- [ ] Prioritized backlog and proposed implementation plan are supplied.
- [ ] No source, project, external account, or production changes were made during the audit.
- [ ] Claude stopped and waited for explicit approval.

### Implementation acceptance

- [ ] Only owner-approved tasks were implemented.
- [ ] Every intended indexable public route has correct response, rendered content, canonical, robots behavior, and unique page metadata appropriate to its purpose.
- [ ] Sitemap and robots configuration are correct for the verified environment; private/sensitive routes are not inadvertently indexable.
- [ ] Internal links expose all key pages and use meaningful anchor text.
- [ ] Any structured data is accurate, relevant, and validated; invalid/unjustified schema is removed or not added.
- [ ] Developer page meets the applicable type-specific criteria and has independent validation evidence.
- [ ] Relevant lint/type/build/test checks pass, or failures are listed with impact and next step.
- [ ] Performance is measured; changes target identified bottlenecks and do not cause regressions.
- [ ] No spam tactics, fabricated claims, unsupported keyword metrics, or unapproved third-party actions were introduced.
- [ ] Final report maps each approved requirement to changed files, validation evidence, result, and remaining gaps.

## 13. Required final implementation report

After approved work, return a concise but complete report containing:

1. Executive summary: completed, not completed, and highest remaining risks.
2. Before/after status matrix against the original audit.
3. Changed files/routes, with the purpose of each significant change.
4. Validation commands and outcomes, including build/lint/types/tests and SEO-specific checks.
5. Developer page before/after findings and evidence.
6. URLs or local test steps needed for manual verification.
7. External steps not performed because permission/access was unavailable.
8. Current SEO baseline and metrics to monitor, if data was available.
9. Next 5 highest-priority actions with rationale.
10. An explicit statement confirming whether deployment or external account changes occurred.

## 14. Copy-ready operating instruction for Claude Code

> You are the senior technical SEO engineer and implementation agent for the Eflow repository currently open in this workspace. Your objective is to maximize sustainable, relevant organic search visibility and qualified traffic, with special priority for the site’s actual Developer page.
>
> **FIRST RUN A READ-ONLY AUDIT. DO NOT IMPLEMENT YET.** Inspect the repository, instructions, routes, rendering framework, current domain/environment, SEO components, sitemap, robots rules, canonicals, metadata, internal links, structured data, accessibility, performance, tests, and the live website where safely available. Check the current Git status first and preserve all pre-existing changes. Do not install packages, modify files, run formatters that rewrite files, submit URLs, change external platforms, or deploy.
>
> Discover the actual purpose of Eflow and the Developer page from evidence. Do not assume its business category, user audience, domain, technologies, profile identity, or features. Do not reuse SEO content or assumptions from another product. Inspect whether the Developer page is a personal profile/portfolio, developer documentation/API page, or another type, and apply the correct branch of this PRD.
>
> Produce an evidence-based audit report for the owner. Classify every requirement as Implemented, Partial, Missing, Not applicable, or Unverified/Blocked. Cite file paths/lines, route URLs, command outputs, or observed behavior. Separate facts from hypotheses. Never invent search volume, rankings, traffic metrics, competitor findings, or access to Search Console/analytics. Give the Developer page its own deep-dive section. Prioritize findings by impact, confidence, effort, dependency, and risk. Propose a concrete implementation plan and tests.
>
> **AFTER THE REPORT, STOP. Make no changes until the owner explicitly approves implementation.** The owner may approve the entire plan or a subset. If they approve only a subset, implement only that scope. This approval gate is mandatory.
>
> After approval, execute the approved changes carefully and incrementally. Follow Google Search Essentials and current official Search Central documentation. Focus on real technical blockers, useful original content, page-specific metadata, stable URLs, crawlable links, valid sitemap/canonical behavior, appropriate rendering, factual structured data, measured Core Web Vitals, and legitimate authority-building recommendations. Prioritize the Developer page separately. Do not use keyword stuffing, doorway pages, scaled low-value AI content, fake reviews/credentials, paid manipulative links, cloaking, or other spam. Preserve the site’s product behavior and design.
>
> Run relevant project tests and SEO validation; report exact results, changed files, remaining issues, and any manual steps that require the owner. Do not deploy or make external account changes unless separately authorized. Do not promise ranking positions or traffic results.

## 15. Official reference documentation

Use current official documentation and verify guidance at implementation time. These references are the baseline, not a substitute for inspecting Eflow.

- Google Search Essentials: https://developers.google.com/search/docs/essentials
- SEO Starter Guide: https://developers.google.com/search/docs/fundamentals/seo-starter-guide
- SEO guide for developers: https://developers.google.com/search/docs/fundamentals/get-started-developers
- Helpful, reliable, people-first content: https://developers.google.com/search/docs/fundamentals/creating-helpful-content
- Google Search spam policies: https://developers.google.com/search/docs/essentials/spam-policies
- JavaScript SEO basics: https://developers.google.com/search/docs/crawling-indexing/javascript/javascript-seo-basics
- Build and submit a sitemap: https://developers.google.com/search/docs/crawling-indexing/sitemaps/build-sitemap
- Crawlable links: https://developers.google.com/search/docs/crawling-indexing/links-crawlable
- Core Web Vitals and Search: https://developers.google.com/search/docs/appearance/core-web-vitals
- Structured data guidelines: https://developers.google.com/search/docs/appearance/structured-data/sd-policies
- ProfilePage structured data (only if the Developer page genuinely qualifies): https://developers.google.com/search/docs/appearance/structured-data/profile-page
- Get started with Search Console: https://developers.google.com/search/docs/monitor-debug/search-console-start

---

**Final operating principle:** Eflow’s SEO should be driven by verified site conditions, real user intent, accurate claims, strong technical execution, and measurable outcomes. Audit first; show the evidence; wait for approval; then implement and validate. High ambition is useful. Blind changes are just expensive guessing.
