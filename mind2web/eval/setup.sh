#!/bin/bash
# mind2web/eval/setup.sh
# Sets up the Online-Mind2Web evaluation environment.
# Usage: bash mind2web/eval/setup.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EVAL_DIR="$SCRIPT_DIR/Online-Mind2Web"

# Clone if not already present
if [ ! -d "$EVAL_DIR" ]; then
  echo "Cloning Online-Mind2Web evaluation repo..."
  git clone https://github.com/OSU-NLP-Group/Online-Mind2Web.git "$EVAL_DIR"
else
  echo "Evaluation repo already cloned at $EVAL_DIR"
fi

# Check for conda
if ! command -v conda &> /dev/null; then
  echo "conda not found. Install Miniconda/Anaconda first."
  echo "  brew install miniconda  (macOS)"
  exit 1
fi

# Create conda env if not exists
if ! conda env list | grep -q "mind2web"; then
  echo "Creating conda environment 'mind2web'..."
  conda create -n mind2web python=3.11 -y
fi

echo "Installing Python dependencies..."
conda run -n mind2web pip install -r "$EVAL_DIR/requirements.txt"

echo ""
echo "Setup complete. To run evaluation:"
echo ""
echo "  conda activate mind2web"
echo "  cd $EVAL_DIR"
echo "  python src/run.py \\"
echo "    --mode WebJudge_Online_Mind2Web_eval \\"
echo "    --model o4-mini \\"
echo "    --trajectories_dir ../../results \\"
echo "    --api_key \$OPENAI_API_KEY \\"
echo "    --output_path ../../eval_results \\"
echo "    --num_worker 10 \\"
echo "    --score_threshold 3"
