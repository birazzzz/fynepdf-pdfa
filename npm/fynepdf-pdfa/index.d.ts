export interface PdfaOptions {
  /** PDF/A-1b, 2b (the default) or 3b. */
  part?: 1 | 2 | 3;
  /** The substitute fonts' folder; the package's own by default. */
  fonts?: string;
  /** Resolution of PDF/A-1b pages that use transparency, drawn as pictures (default 300). */
  flattenDpi?: number;
  /** Stops the conversion. */
  signal?: AbortSignal;
}

export interface PdfaResult {
  ok: boolean;
  /** When not ok: "unreadable", "password" or "write". */
  error?: string;
  /** The output is PDF/A. When false it is still written, without the PDF/A label, and problems say why. */
  conforms: boolean;
  pages: number;
  /** Signatures removed: a signed file can't be changed without breaking them. */
  signatures: number;
  /** Why the file isn't PDF/A: "code" or "code: detail", such as "font-not-embedded: Arial". */
  problems: string[];
  /** What was changed, by kind: "javascript", "cmyk-colours", "font-widths-written", ... */
  changes: Record<string, number>;
}

export interface PdfaError extends Error {
  /** "unreadable", "password", "write", "options" or "failed". */
  code: string;
  result?: PdfaResult;
}

/**
 * Converts input to PDF/A and writes output. Resolves even when the file couldn't be made PDF/A
 * (check conforms); rejects with a PdfaError when it can't be read, needs a password, or can't be
 * written.
 */
export function toPdfa(input: string, output: string, options?: PdfaOptions): Promise<PdfaResult>;

/** The command line's path for this platform (or FYNEPDF_PDFA_BINARY). */
export function binaryPath(): string;
