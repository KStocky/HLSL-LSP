using System;
using System.Collections.Generic;
using Microsoft.VisualStudio;
using Microsoft.VisualStudio.Editor;
using Microsoft.VisualStudio.Shell;
using Microsoft.VisualStudio.Shell.Interop;
using Microsoft.VisualStudio.Text;
using Microsoft.VisualStudio.TextManager.Interop;
using Microsoft.VisualStudio.Threading;

namespace HlslLsp.VisualStudio;

internal sealed class HlslNavigationBarManager
{
    private readonly IVsUIShell uiShell;
    private readonly IVsEditorAdaptersFactoryService editorAdapters;
    private readonly ITextDocumentFactoryService textDocuments;
    private readonly HlslLanguageClient languageClient;
    private readonly JoinableTaskFactory joinableTaskFactory;
    private readonly IServiceProvider serviceProvider;
    private readonly Dictionary<IVsCodeWindow, HlslNavigationBarClient> clients =
        new();

    internal HlslNavigationBarManager(
        IVsUIShell uiShell,
        IVsEditorAdaptersFactoryService editorAdapters,
        ITextDocumentFactoryService textDocuments,
        HlslLanguageClient languageClient,
        JoinableTaskFactory joinableTaskFactory,
        IServiceProvider serviceProvider)
    {
        this.uiShell = uiShell;
        this.editorAdapters = editorAdapters;
        this.textDocuments = textDocuments;
        this.languageClient = languageClient;
        this.joinableTaskFactory = joinableTaskFactory;
        this.serviceProvider = serviceProvider;
    }

    internal void AttachToOpenDocuments()
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        ErrorHandler.ThrowOnFailure(
            uiShell.GetDocumentWindowEnum(out var windows));
        var frames = new IVsWindowFrame[1];
        while (windows.Next(1, frames, out var fetched) == VSConstants.S_OK &&
               fetched == 1)
        {
            Attach(frames[0]);
        }
    }

    private void Attach(IVsWindowFrame frame)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        if (ErrorHandler.Failed(
                frame.GetProperty(
                    (int)__VSFPROPID.VSFPROPID_DocView,
                    out var rawView)) ||
            rawView is not IVsCodeWindow codeWindow ||
            clients.ContainsKey(codeWindow) ||
            codeWindow is not IVsDropdownBarManager dropdownManager ||
            ErrorHandler.Failed(codeWindow.GetBuffer(out var textLines)))
        {
            return;
        }

        var buffer = editorAdapters.GetDataBuffer(textLines);
        if (buffer == null ||
            (!buffer.ContentType.IsOfType("HLSL-LSP-Colored") &&
             !buffer.ContentType.IsOfType("HLSLHeader-LSP-Colored")) ||
            !textDocuments.TryGetTextDocument(buffer, out var document))
        {
            return;
        }

        ErrorHandler.ThrowOnFailure(
            dropdownManager.GetDropdownBar(out var existing));
        if (existing != null)
        {
            return;
        }

        var client = new HlslNavigationBarClient(
            dropdownManager,
            codeWindow,
            buffer,
            editorAdapters,
            languageClient,
            joinableTaskFactory,
            serviceProvider,
            document.FilePath,
            OnClientClosed);
        var result = dropdownManager.AddDropdownBar(2, client);
        if (ErrorHandler.Failed(result))
        {
            client.Dispose();
            ErrorHandler.ThrowOnFailure(result);
        }
        clients.Add(codeWindow, client);
    }

    private void OnClientClosed(HlslNavigationBarClient client)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        IVsCodeWindow closedWindow = null;
        foreach (var entry in clients)
        {
            if (ReferenceEquals(entry.Value, client))
            {
                closedWindow = entry.Key;
                break;
            }
        }
        if (closedWindow != null)
        {
            clients.Remove(closedWindow);
        }
    }

    internal void RemoveAll()
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        var activeClients = new List<HlslNavigationBarClient>(clients.Values);
        clients.Clear();
        foreach (var client in activeClients)
        {
            client.Dispose();
        }
    }
}
