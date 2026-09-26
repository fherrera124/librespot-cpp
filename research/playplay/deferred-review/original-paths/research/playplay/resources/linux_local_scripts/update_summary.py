import re

with open("research/playplay/docs/WINDOWS_WORK_SUMMARY.md", "r") as f:
    content = f.read()

new_content = content.replace(
    "2.  **Validación de los 11 Vectores:**\n    Usando el nuevo approach de Frida, se debe pasar la batería completa de 11 vectores (`ground-truth-vectors.json`) para verificar la transformación perfecta.",
    """2.  **Validación de los 11 Vectores:**
    *Completado (23-Sep-2026):* 
    *   Se descubrió un bug crítico: `VmObjectTransform` muta el estado interno de `vm_obj`. Si se pasan múltiples vectores consecutivamente sin re-inicializar o restaurar la estructura, provoca un *Access Violation* (`system error`).
    *   Además, interceptar el hilo de audio y usar bloqueos asíncronos (`op.wait()`) impedía recibir los resultados en Python a tiempo.
    *   **LA SOLUCIÓN:** Se desarrolló `validate_all_in_js.py`. Este script inyecta la batería completa de vectores directamente en JavaScript. Al interceptar el primer pase (`onEnter`), hace una copia de seguridad de los 144 bytes de `vm_obj` y, para cada vector, restaura la memoria antes de invocar `VmObjectTransform`. Esto validó exitosamente los 11 vectores del Token E."""
)

with open("research/playplay/docs/WINDOWS_WORK_SUMMARY.md", "w") as f:
    f.write(new_content)
