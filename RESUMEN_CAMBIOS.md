# Resumen de Cambios - Configuración de Radares en VideoHub

## Problema Resuelto

**Problema Original**: Cuando se hacen cambios en el video hub, no se estaban configurando los radares en los grupos correctamente. Si el video hub se ha movido, era posible que cada vez que se aplicara una configuración, los radares no quedaran bien asignados.

**Solución Implementada**: Se ha agregado soporte para incluir los radares en las configuraciones de manera simple para estar sobreseguros.

## Cambios Realizados

### 1. Estructura de Configuración Actualizada (config.json)

Se agregó una nueva sección `configurations` que permite definir múltiples configuraciones (config_a, config_b, config_c, etc.), cada una con:

- **g1_g4**: Cámaras del grupo 1-4
- **g5_g8**: Cámaras del grupo 5-8
- **radars**: Radares asignados (nuevo campo)

Ejemplo:
```json
"configurations": {
  "config_a": {
    "g1_g4": [1, 2, 3, 12],
    "g5_g8": [4, 5, 6, 7],
    "radars": [13, 14, 15, 16]
  },
  "config_b": {
    "g1_g4": [1, 2, 3, 4],
    "g5_g8": [5, 6, 7, 8],
    "radars": [13, 14, 15, 16]
  },
  "config_c": {
    "g1_g4": [9, 10, 11, 12],
    "g5_g8": [1, 2, 3, 4],
    "radars": [13, 14, 15, 16]
  }
}
```

### 2. Nuevo Método en VideoHubClient

Se agregó el método `SetRadarRouting()` que:
- Valida los índices de los radares
- Verifica que estén configurados correctamente
- Registra la configuración en los logs para diagnóstico

### 3. Aplicación Automática de Configuraciones

Durante la Fase 1 de inicialización:
- Se carga la configuración por defecto (config_a)
- Se aplica el routing de radares automáticamente
- Se validan todos los índices antes de aplicar
- Se registra cada paso en los logs

### 4. Funciones de Ayuda

Se creó `ApplyVideoHubConfiguration()` que:
- Aplica una configuración específica al VideoHub
- Configura los radares correspondientes
- Maneja errores de forma robusta
- Registra advertencias si los radares no están definidos

## Ventajas del Nuevo Sistema

1. **Seguridad**: Los radares se configuran explícitamente en cada configuración
2. **Trazabilidad**: Todos los cambios se registran en los logs
3. **Validación**: Se verifican los índices antes de aplicar cambios
4. **Flexibilidad**: Fácil agregar nuevas configuraciones (config_d, config_e, etc.)
5. **Mantenibilidad**: Código más claro y estructurado

## Cómo Usar

### Para Agregar una Nueva Configuración

1. Editar `config.json`
2. Agregar una nueva entrada en `configurations`:
   ```json
   "config_d": {
     "g1_g4": [5, 6, 7, 8],
     "g5_g8": [9, 10, 11, 12],
     "radars": [13, 14, 15, 16]
   }
   ```
3. El sistema la cargará automáticamente

### Para Verificar la Configuración

Ejecutar el sistema en modo TEST (opción 1 del menú):
- Se aplicará config_a por defecto
- Se validarán todos los puertos
- Se verificarán los radares
- Se mostrarán mensajes en los logs

## Archivos Modificados

1. **src/config.json**: Estructura actualizada con sección `configurations`
2. **src/control/VideoHubClient.h**: Agregado método `SetRadarRouting()`
3. **src/control/VideoHubClient.cpp**: Implementación de validación de radares
4. **src/core/main.cpp**: 
   - Estructura `VideoHubConfig` para configuraciones
   - Función `ApplyVideoHubConfiguration()` 
   - Actualización de `LoadConfig()` para parsear configuraciones
   - Actualización de `RunPhase1()` para aplicar configuraciones

## Documentación

Se creó **CONFIGURATION.md** con:
- Guía completa de la estructura de configuración
- Ejemplos de uso
- Mejores prácticas
- Solución de problemas
- Guía de migración desde versiones anteriores

## Próximos Pasos Recomendados

1. **Pruebas**: Ejecutar el sistema con diferentes configuraciones para verificar el routing
2. **Validación**: Confirmar que los radares se configuran correctamente en cada cambio
3. **Expansión**: Considerar agregar más configuraciones según necesidades del proyecto
4. **Monitoreo**: Revisar los logs durante las primeras ejecuciones para confirmar el comportamiento

## Notas Técnicas

- Los radares (puertos 13-16) se definen explícitamente en cada configuración
- El sistema aplica config_a por defecto durante la inicialización
- Si config_a no existe, se usa la primera configuración disponible
- La falta de radares en una configuración genera una advertencia pero no es fatal
- Todos los cambios son retrocompatibles con el resto del sistema
