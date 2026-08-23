using System;
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
    private readonly IVsEditorAdaptersFactoryService editorAdapters;
    private readonly ITextDocumentFactoryService textDocuments;
    private readonly HlslLanguageClient languageClient;
    private readonly JoinableTaskFactory joinableTaskFactory;
    private readonly IServiceProvider serviceProvider;
    private HlslNavigationBarClient client;

    internal HlslNavigationBarManager(
        IVsEditorAdaptersFactoryService editorAdapters,
        ITextDocumentFactoryService textDocuments,
        HlslLanguageClient languageClient,
        JoinableTaskFactory joinableTaskFactory,
        IServiceProvider serviceProvider)
    {
        this.editorAdapters = editorAdapters;
        this.textDocuments = textDocuments;
        this.languageClient = languageClient;
        this.joinableTaskFactory = joinableTaskFactory;
        this.serviceProvider = serviceProvider;
    }

    internal void AttachToDocumentFrame(object value)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        if (value is IVsWindowFrame frame)
        {
            Attach(frame);
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
            codeWindow is not IVsDropdownBarManager dropdownManager ||
            ErrorHandler.Failed(codeWindow.GetBuffer(out var textLines)))
        {
            return;
        }

        var buffer = editorAdapters.GetDataBuffer(textLines);
        if (client != null &&
            buffer != null &&
            textDocuments.TryGetTextDocument(buffer, out var activeDocument) &&
            client.Matches(buffer, activeDocument.FilePath))
        {
            client.Activate(dropdownManager);
            return;
        }

        if (buffer == null ||
            (!buffer.ContentType.IsOfType("HLSL-LSP-Colored") &&
             !buffer.ContentType.IsOfType("HLSLHeader-LSP-Colored")) ||
            !textDocuments.TryGetTextDocument(buffer, out var document))
        {
            return;
        }

        ErrorHandler.ThrowOnFailure(
            dropdownManager.GetDropdownBar(out var existing));
        if (client != null)
        {
            var activeClient = client;
            client = null;
            activeClient.Detach();
            activeClient.Rebind(
                dropdownManager,
                codeWindow,
                buffer,
                document.FilePath);
            ErrorHandler.ThrowOnFailure(
                dropdownManager.GetDropdownBar(out var remainingBar));
            if (remainingBar != null)
            {
                ErrorHandler.ThrowOnFailure(
                    dropdownManager.RemoveDropdownBar());
            }
            ErrorHandler.ThrowOnFailure(
                dropdownManager.AddDropdownBar(2, activeClient));
            ErrorHandler.ThrowOnFailure(
                dropdownManager.GetDropdownBar(out var reboundBar));
            if (reboundBar == null)
            {
                throw new InvalidOperationException(
                    "Visual Studio did not return the rebound HLSL navigation bar.");
            }
            activeClient.SetDropdownBar(reboundBar);
            client = activeClient;
            return;
        }
        if (existing != null)
        {
            return;
        }

        var addedClient = new HlslNavigationBarClient(
            dropdownManager,
            codeWindow,
            buffer,
            editorAdapters,
            languageClient,
            joinableTaskFactory,
            serviceProvider,
            document.FilePath,
            OnClientClosed);
        var result = dropdownManager.AddDropdownBar(2, addedClient);
        if (ErrorHandler.Failed(result))
        {
            addedClient.Dispose();
            ErrorHandler.ThrowOnFailure(result);
        }
        if (!addedClient.HasDropdownBar)
        {
            ErrorHandler.ThrowOnFailure(
                dropdownManager.GetDropdownBar(out var addedBar));
            if (addedBar == null)
            {
                addedClient.Dispose();
                throw new InvalidOperationException(
                    "Visual Studio did not return the added HLSL navigation bar.");
            }
            addedClient.SetDropdownBar(addedBar);
        }
        client = addedClient;
    }

    private void OnClientClosed(HlslNavigationBarClient closedClient)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        if (ReferenceEquals(client, closedClient))
        {
            client = null;
        }
    }

    internal void RemoveAll()
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        var activeClient = client;
        client = null;
        activeClient?.Dispose();
    }
}
