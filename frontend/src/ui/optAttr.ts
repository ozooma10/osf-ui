export function optAttr<T>(
  name: string,
  value: T | undefined | null | false | '',
): Record<string, T> {
  return value ? { [name]: value as T } : {};
}
