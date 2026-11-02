# Model context discovery (#242)

The old model picker discovered capacity but did not refresh existing routes.
Large-provider listings were limited to 200 entries, hiding some configured models.

The Models table now offers **核实上下文** using the saved route credentials.
Discovery updates and persists only matching provider/endpoint/model routes.
Explicit manual configuration wins over automatic refresh. Missing metadata
retains known capacities and remains honestly marked unverified for unknown models.
Listings retain up to 10,000 models; oversized responses fail explicitly instead
of saving truncated JSON. OpenRouter nested output limits are supported.

NVIDIA's exact `z-ai/glm-5.3-flash` deployment has a verified 1,048,576-token
catalog entry, based on its [official model card](https://build.nvidia.com/z-ai/glm-5-3-flash/modelcard).
Unknown proxy endpoints/aliases do not inherit this capacity.

Validation: metadata integration fixture includes a target after 250 distractors,
saved-key authentication, manual override preservation and restart persistence.
UI regression checks existing-route refresh. A real configured OpenRouter API
query through the isolated Runtime returned 416 models and persisted
`nvidia/nemotron-3.5-super-vl:free`: context 1,000,000, output 131,072 tokens,
source `provider_metadata`. No generation request or token charge was incurred.
Production routes were not modified by the test.
