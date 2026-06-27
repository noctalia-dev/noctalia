#!/usr/bin/env -S bash

# Pull translations from Noctalia Translate API
# Usage: ./i18n-pull.sh <project-slug> <output-dir>
# Example: ./i18n-pull.sh noctalia ./assets/translations

set -e

# Configuration
API_BASE="${I18N_API_BASE:-https://i18n.noctalia.dev}"
PROJECT_SLUG="${1:-noctalia}"
OUTPUT_DIR="${2:-./assets/translations}"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${YELLOW}Pulling translations for project: ${PROJECT_SLUG}${NC}"
echo "API: ${API_BASE}"
echo "Output: ${OUTPUT_DIR}"
echo ""

# Confirmation
read -p "Pull translations and overwrite local files? [y/N] " -n 1 -r
echo ""
if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    echo "Aborted."
    exit 0
fi
echo ""

# Create output directory if it doesn't exist
mkdir -p "$OUTPUT_DIR"

# Pull all translations
RESPONSE=$(curl -s -w "\n%{http_code}" "${API_BASE}/api/projects/${PROJECT_SLUG}/pull")
HTTP_CODE=$(echo "$RESPONSE" | tail -n1)
BODY=$(echo "$RESPONSE" | sed '$d')

if [ "$HTTP_CODE" != "200" ]; then
    echo -e "${RED}Error: HTTP ${HTTP_CODE}${NC}"
    echo "$BODY"
    exit 1
fi

# Check if jq is available
if ! command -v jq &> /dev/null; then
    echo -e "${RED}Error: jq is required but not installed${NC}"
    echo "Install with: apt install jq (Linux) or brew install jq (macOS)"
    exit 1
fi

# Get list of locales
LOCALES=$(echo "$BODY" | jq -r 'keys[]')

if [ -z "$LOCALES" ]; then
    echo -e "${RED}Error: No translations found${NC}"
    exit 1
fi

# Save each locale to a separate file
COUNT=0
for LOCALE in $LOCALES; do
    OUTPUT_FILE="${OUTPUT_DIR}/${LOCALE}.json"
    echo "$BODY" | jq ".[\"${LOCALE}\"]" > "$OUTPUT_FILE"
    echo -e "${GREEN}Saved: ${OUTPUT_FILE}${NC}"
    COUNT=$((COUNT + 1))
done

echo ""
echo -e "${GREEN}Successfully pulled ${COUNT} language(s)${NC}"
