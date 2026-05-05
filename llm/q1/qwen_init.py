import time
from mlx_lm import load, generate

print("--- Qwen2.5-Coder 32B 로드 시작 (128GB M4 Mac 최적화) ---")
start_time = time.time()

# 고정밀 8-bit 모델 로드
model_path = "mlx-community/Qwen2.5-Coder-32B-Instruct-8bit"
model, tokenizer = load(model_path)

end_time = time.time()
print(f"--- 모델 로드 완료 (소요 시간: {end_time - start_time:.2f}초) ---")

prompt = "Hello, Qwen. You are now the core intelligence of Orange AI. Introduce yourself to the CEO (Head) and confirm your readiness."

print("\n[Qwen의 첫 인사]:")
response = generate(model, tokenizer, prompt=prompt, verbose=True, max_tokens=512)
